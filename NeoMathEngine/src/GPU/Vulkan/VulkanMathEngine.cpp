﻿/* Copyright © 2017-2020 ABBYY Production LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

	http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
--------------------------------------------------------------------------------------------------------------*/

#include <common.h>
#pragma hdrstop

#include <NeoMathEngine/NeoMathEngineDefs.h>

#ifdef NEOML_USE_VULKAN

#include <algorithm>
#include <DllLoader.h>
#include <VulkanMathEngine.h>
#include <MathEngineCommon.h>
#include <MathEngineDeviceStackAllocator.h>
#include <MathEngineHostStackAllocator.h>
#include <MemoryHandleInternal.h>
#include <VulkanDll.h>
#include <VulkanCommandQueue.h>
#include <VulkanShader.h>

namespace NeoML {

// Include the shaders code
#include <shaders/generated/VectorToImage.h>

// The maximum number of bytes that may be asynchronously copied into GPU
static const size_t VulkanMaxExchangeAsyncSize = 65536;

// The size of the buffer for data exchange
static const size_t VulkanExchangeBufferSize = 4 * 1024 * 1024; // 4 MB

// The maximum number of groups over the X dimension when working with a 1D (vector) shader
// With larger sizes, the shader data will be represented in two dimensions
static const int VulkanMaxVectorXGroupCount = 8192;

//------------------------------------------------------------------------------------------------------------

static inline void getDeviceInfo( const CVulkanDll& dll, int index, CMathEngineInfo& info )
{
	info.Type = MET_Vulkan;
	::memset( info.Name, 0, sizeof( info.Name ) );
	::strcpy( info.Name, dll.GetDevices()[index].Properties.deviceName );
	info.Id = index;
	info.AvailableMemory = dll.GetDevices()[index].AvailableMemory;
}

bool LoadVulkanEngineInfo( CVulkanDll& dll, std::vector< CMathEngineInfo, CrtAllocator<CMathEngineInfo> >& result )
{
	for( int i = 0; i < static_cast<int>( dll.GetDevices().size() ); i++ ) {
		CMathEngineInfo info;
		getDeviceInfo( dll, i, info );
		result.push_back( info );
	}

	return !result.empty();
}

//------------------------------------------------------------------------------------------------------------

inline int Ceil( int val, int discret )
{
	assert( discret > 0 );
	if( val > 0 ) {
		return ( val + discret - 1 ) / discret;
	}
	return val / discret;
}

//------------------------------------------------------------------------------------------------------------

const int VulkanMemoryAlignment = 16;

CVulkanMathEngine::CVulkanMathEngine( CVulkanDll& _dll, int _deviceNumber, size_t memoryLimit ) :
	dll( _dll ),
	deviceNumber( _deviceNumber ),
	device( dll.CreateDevice( dll.GetDevices()[_deviceNumber] ) )
{
	ASSERT_EXPR( device != 0 ); // failed to create the device
	shaderLoader = std::unique_ptr<CVulkanShaderLoader>( new CVulkanShaderLoader( *device ) );
	commandQueue = std::unique_ptr<CVulkanCommandQueue>( new CVulkanCommandQueue( *device ) );
	memoryLimit = min( memoryLimit == 0 ? SIZE_MAX : memoryLimit, dll.GetDevices()[deviceNumber].AvailableMemory );
	memoryPool = std::unique_ptr<CMemoryPool>( new CMemoryPool( memoryLimit, this, false ) );
	deviceStackAllocator = std::unique_ptr<CDeviceStackAllocator>( new CDeviceStackAllocator( *memoryPool, VulkanMemoryAlignment ) );
	hostStackAllocator = std::unique_ptr<CHostStackAllocator>( new CHostStackAllocator( VulkanMemoryAlignment ) );
	tmpImages.insert( tmpImages.end(), TVI_Count, 0 );

	CDllLoader::Load(CDllLoader::VULKAN_DLL);
}

CVulkanMathEngine::~CVulkanMathEngine()
{
	for( auto cur : tmpImages ) {
		delete cur;
	}
	CDllLoader::Free(CDllLoader::VULKAN_DLL);
}

void CVulkanMathEngine::SetReuseMemoryMode( bool enable )
{
	std::lock_guard<std::mutex> lock( mutex );
	memoryPool->SetReuseMemoryMode( enable );
}

CMemoryHandle CVulkanMathEngine::HeapAlloc( size_t size )
{
	std::lock_guard<std::mutex> lock( mutex );
	CMemoryHandle result = memoryPool->Alloc( size );
	if( result.IsNull() ) {
		THROW_MEMORY_EXCEPTION;
	}

	return result;
}

void CVulkanMathEngine::HeapFree( const CMemoryHandle& handle )
{
	ASSERT_EXPR( handle.GetMathEngine() == this );

	std::lock_guard<std::mutex> lock( mutex );
	return memoryPool->Free( handle );
}

CMemoryHandle CVulkanMathEngine::StackAlloc( size_t size )
{
	std::lock_guard<std::mutex> lock( mutex );
	CMemoryHandle result = deviceStackAllocator->Alloc( size );
	if( result.IsNull() ) {
		THROW_MEMORY_EXCEPTION;
	}
	return result;
}

void CVulkanMathEngine::StackFree( const CMemoryHandle& ptr )
{
	std::lock_guard<std::mutex> lock( mutex );
	deviceStackAllocator->Free( ptr );
}

size_t CVulkanMathEngine::GetFreeMemorySize() const
{
	std::lock_guard<std::mutex> lock( mutex );
	return memoryPool->GetFreeMemorySize();
}

size_t CVulkanMathEngine::GetPeakMemoryUsage() const
{
	std::lock_guard<std::mutex> lock( mutex );
	return memoryPool->GetPeakMemoryUsage();
}

void CVulkanMathEngine::CleanUp()
{
	std::lock_guard<std::mutex> lock( mutex );
	deviceStackAllocator->CleanUp();
	hostStackAllocator->CleanUp();
	commandQueue->CleanUp();
	for( auto& cur : tmpImages ) {
		delete cur;
		cur = 0;
	}
	memoryPool->CleanUp();
}

void* CVulkanMathEngine::GetBuffer( const CMemoryHandle& handle, size_t pos, size_t size )
{
	ASSERT_EXPR(handle.GetMathEngine() == this);

	size_t realSize = size + 16;
	char* result = reinterpret_cast<char*>( hostStackAllocator->Alloc( realSize ) );
	size_t* posPtr = reinterpret_cast<size_t*>( result );
	*posPtr = pos;
	size_t* sizePtr = reinterpret_cast<size_t*>( result ) + 1;
	*sizePtr = size;
	DataExchangeRaw( result + 16, handle, size );
	return result + 16;
}

void CVulkanMathEngine::ReleaseBuffer( const CMemoryHandle& handle, void* ptr, bool exchange )
{
	ASSERT_EXPR(handle.GetMathEngine() == this);

	if( exchange ) {
		size_t* posPtr = reinterpret_cast<size_t*>( reinterpret_cast<char*>( ptr ) - 16 );
		size_t pos = *posPtr;
		size_t* sizePtr = posPtr + 1;
		size_t size = *sizePtr;

		DataExchangeRaw( CTypedMemoryHandle<char>( handle ) + pos, ptr, size );
	}

	hostStackAllocator->Free( reinterpret_cast<char*>( ptr ) - 16 );
}

void CVulkanMathEngine::DataExchangeRaw( const CMemoryHandle& to, const void* from, size_t size )
{
	ASSERT_EXPR( to.GetMathEngine() == this );

	CTypedMemoryHandle<char> toPtr( to );
	const char* fromPtr = reinterpret_cast<const char*>( from );

	std::lock_guard<std::mutex> lock( mutex );
	while( size != 0 ) {
		CVulkanMemory* vulkanMemory = GetRawAllocation( toPtr );
		ptrdiff_t vulkanOffset = GetRawOffset( toPtr );

		if( size <= VulkanMaxExchangeAsyncSize ) {
			// We can write the data asynchronously
			commandQueue->RunUpdateBuffer( vulkanMemory->Buffer, vulkanOffset, fromPtr, size );
			break;
		}

		commandQueue->Wait(); // make sure that the exchange buffer is not used by anyone else

		size_t toCopy = size;
		if( toCopy > VulkanExchangeBufferSize ) {
			toCopy = VulkanExchangeBufferSize;
		}

		void* mappedData = 0;
		vkSucceded( device->vkMapMemory( device->Handle, vulkanMemory->Memory, vulkanOffset, toCopy, 0, &mappedData ) );
		memcpy( mappedData, fromPtr, toCopy );
		device->vkUnmapMemory( device->Handle, vulkanMemory->Memory );

		size -= toCopy;
		toPtr += toCopy;
		fromPtr += toCopy;
	}
}

void CVulkanMathEngine::DataExchangeRaw( void* to, const CMemoryHandle& from, size_t size )
{
	ASSERT_EXPR( from.GetMathEngine() == this );

	CTypedMemoryHandle<char> fromPtr( from );
	char* toPtr = reinterpret_cast<char*>( to );

	std::lock_guard<std::mutex> lock( mutex );
	while( size != 0 ) {
		CVulkanMemory* vulkanMemory = GetRawAllocation( fromPtr );
		ptrdiff_t vulkanOffset = GetRawOffset( fromPtr );

		size_t toCopy = size;
		if( toCopy > VulkanExchangeBufferSize ) {
			toCopy = VulkanExchangeBufferSize;
		}

		commandQueue->Wait(); // wait for the data to be written into the exchange buffer

		void* mappedData = 0;
		vkSucceded( device->vkMapMemory( device->Handle, vulkanMemory->Memory, vulkanOffset, toCopy, 0, &mappedData ) );
		memcpy( toPtr, mappedData, toCopy );
		device->vkUnmapMemory( device->Handle, vulkanMemory->Memory );

		size -= toCopy;
		fromPtr += toCopy;
		toPtr += toCopy;
	}
}

CMemoryHandle CVulkanMathEngine::CopyFrom( const CMemoryHandle& handle, size_t size )
{
	CMemoryHandle result = HeapAlloc( size );

	IMathEngine* otherMathEngine = handle.GetMathEngine();
	void* ptr = otherMathEngine->GetBuffer( handle, 0, size );

	DataExchangeRaw( result, ptr, size );

	otherMathEngine->ReleaseBuffer( handle, ptr, false );

	return result;
}

CMemoryHandle CVulkanMathEngine::Alloc( size_t size )
{
	std::unique_ptr<CVulkanMemory> vulkanMemory( new CVulkanMemory );

	VkBufferCreateInfo createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	createInfo.size = size;
	createInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VkBuffer buffer = 0;
	vkSucceded( device->vkCreateBuffer( device->Handle, &createInfo, 0, &buffer ) );

	VkMemoryRequirements req = {};
	device->vkGetBufferMemoryRequirements( device->Handle, buffer, &req );

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = req.size;

	// Look for the suitable index in memory
	bool isFound = false;
	const int flags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

	for( int i = 0; i < (int)device->MemoryProperties.memoryTypeCount; ++i ) {
		if((req.memoryTypeBits & (1 << i)) != 0 &&
			(int)(device->MemoryProperties.memoryTypes[i].propertyFlags & flags) == flags) {
			allocInfo.memoryTypeIndex = i;
			isFound = true;
			break;
		}
	}
	assert(isFound);

	VkDeviceMemory memory;
	if( device->vkAllocateMemory( device->Handle, &allocInfo, 0, &memory ) != VK_SUCCESS ) {
		device->vkDestroyBuffer( device->Handle, buffer, 0 );
		return CMemoryHandle();
	}

	vkSucceded( device->vkBindBufferMemory( device->Handle, buffer, memory, 0 ) );

	vulkanMemory->Buffer = buffer;
	vulkanMemory->Memory = memory;
	return CMemoryHandleInternal::CreateMemoryHandle( &mathEngine(), vulkanMemory.release() );
}

void CVulkanMathEngine::Free( const CMemoryHandle& handle )
{
	ASSERT_EXPR( handle.GetMathEngine() == this );

	// Make sure that the memory we are about to clean is not used
	commandQueue->Wait();

	CVulkanMemory* vulkanMemory = GetRawAllocation(handle);
	device->vkFreeMemory( device->Handle, vulkanMemory->Memory, 0 );
	device->vkDestroyBuffer( device->Handle, vulkanMemory->Buffer, 0 );
	delete vulkanMemory;
}

void CVulkanMathEngine::GetMathEngineInfo( CMathEngineInfo& info ) const
{
	getDeviceInfo( dll, deviceNumber, info );
}

//------------------------------------------------------------------------------------------------------------
// private methods

// Calculate the channels group size (intended for images with large height)
int CVulkanMathEngine::getChannelGroupSize( int height, int channels ) const
{
	assert(height > 0);
	assert(channels > 0);

	if( !device->IsImageBased ) {
		return channels; // no images used, so no limitations on geometric size
	}

	if( height * channels <= static_cast<int>( device->Properties.limits.maxImageDimension2D ) ) {
		return channels; // the image fits into the limitations
	}

	return device->Properties.limits.maxImageDimension2D / height;
}

// Gets a temporary object with the given id and size
const CVulkanImage* CVulkanMathEngine::getTmpImage( TTmpVulkanImage imageId, int width, int height )
{
	assert( device->IsImageBased );

	int newWidth = width;
	int newHeight = height;
	if( tmpImages[imageId] != 0 && tmpImages[imageId]->IsImageFit(newWidth, newHeight) ) {
		tmpImages[imageId]->SetWorkingArea(width, height);
		return tmpImages[imageId];
	}

	if( tmpImages[imageId] != 0 ) {
		commandQueue->Wait(); // make sure the previous temporary image is not used
		delete tmpImages[imageId];
		tmpImages[imageId] = 0;
	}

	CVulkanImage *image = new CVulkanImage( *device, newWidth, newHeight );
	commandQueue->RunChangeLayoutForImage( image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL );
	tmpImages[imageId] = image;

	tmpImages[imageId]->SetWorkingArea(width, height);
	return tmpImages[imageId];
}

const CVulkanImage* CVulkanMathEngine::getTmpImage( TTmpVulkanImage imageId )
{
	assert( device->IsImageBased );
	assert(tmpImages[imageId] != 0);
	return tmpImages[imageId];
}

void CVulkanMathEngine::runShader( const CVulkanShaderData& shader, const void* param, int paramSize,
	const CVulkanImage** images, int imageCount, const CVulkanImage** samplers, int samplerCount,
	const CMemoryHandle* dataBuffers, const size_t* dataSizes, int dataBufferCount,
	int countX, int countY, int countZ )
{
	std::lock_guard<std::mutex> lock( mutex );
	commandQueue->RunComputeShader( shader, Ceil(countX, shader.GroupSizeX), Ceil(countY, shader.GroupSizeY), Ceil(countZ, shader.GroupSizeZ),
		param, paramSize, images, imageCount, samplers, samplerCount,
		dataBuffers, dataSizes, dataBufferCount );
}

void CVulkanMathEngine::runVectorShader( const CVulkanShaderData& shader, const void* param, int paramSize,
	const CVulkanImage** images, int imageCount, const CVulkanImage** samplers, int samplerCount,
	const CMemoryHandle* dataBuffers, const size_t* dataSizes, int dataBufferCount, int count )
{
	int groupCountX = Ceil(count, shader.GroupSizeX);
	int groupCountY = Ceil(groupCountX, VulkanMaxVectorXGroupCount);
	groupCountX = min(groupCountX, VulkanMaxVectorXGroupCount);

	assert(shader.GroupSizeY == 1 && shader.GroupSizeZ == 1);

	std::lock_guard<std::mutex> lock( mutex );
	commandQueue->RunComputeShader( shader, groupCountX, groupCountY, 1,  param, paramSize, images, imageCount, samplers, samplerCount,
		dataBuffers, dataSizes, dataBufferCount );
}

const CVulkanImage& CVulkanMathEngine::batchVectorToImage( int batchSize, const CConstFloatHandle& vector, int size, int imageId )
{
	int size4 = Ceil(size, 4);
	const CVulkanImage* images[] = { getTmpImage((TTmpVulkanImage)imageId, size4, batchSize) };

	CMemoryHandle bufs[1] = { vector };
	size_t sizes[1] = { batchSize * size * sizeof(float) };

	PARAM_STRUCT(VectorToImage) param = { batchSize, size };

	runVectorShader( shaderLoader->GET_SHADER_DATA(VectorToImage, true, 1, 0, 1),
		&param, sizeof(param), images, 1, 0, 0, bufs, sizes, 1, size4 * batchSize );

	return *images[0];
}

} // namespace NeoML

#endif // NEOML_USE_VULKAN
