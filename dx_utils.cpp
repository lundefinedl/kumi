#include "stdafx.h"
#include "dx_utils.hpp"

namespace
{
	HRESULT create_buffer_inner(ID3D11Device* device, const D3D11_BUFFER_DESC& desc, const void* data, ID3D11Buffer** buffer)
	{
		D3D11_SUBRESOURCE_DATA init_data;
		ZeroMemory(&init_data, sizeof(init_data));
		init_data.pSysMem = data;
		return device->CreateBuffer(&desc, data ? &init_data : NULL, buffer);
	}
}

HRESULT create_static_vertex_buffer(ID3D11Device* device, uint32_t vertex_count, uint32_t vertex_size, const void* data, ID3D11Buffer** vertex_buffer) 
{
	return create_buffer_inner(device, CD3D11_BUFFER_DESC(vertex_count * vertex_size, D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_IMMUTABLE), data, vertex_buffer);
}

HRESULT create_dynamic_vertex_buffer(ID3D11Device *device, uint32_t vertex_count, uint32_t vertex_size, ID3D11Buffer** vertex_buffer)
{
	return create_buffer_inner(device, CD3D11_BUFFER_DESC(vertex_count * vertex_size, D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE), NULL, vertex_buffer);
}

void set_vb(ID3D11DeviceContext *context, ID3D11Buffer *buf, uint32_t stride)
{
	UINT ofs[] = { 0 };
	ID3D11Buffer* bufs[] = { buf };
	uint32_t strides[] = { stride };
	context->IASetVertexBuffers(0, 1, bufs, strides, ofs);
}

void screen_to_clip(float x, float y, float w, float h, float *ox, float *oy)
{
	*ox = (x - w / 2) / (w / 2);
	*oy = (h/2 - y) / (h/2);
}

void make_quad_clip_space(float *x, float *y, int stride_x, int stride_y, float center_x, float center_y, float width, float height)
{
	// 0 1
	// 2 3
	x[0*stride_x] = center_x - width;
	y[0*stride_y] = center_y + height;

	x[1*stride_x] = center_x + width;
	y[1*stride_y] = center_y + height;

	x[2*stride_x] = center_x - width;
	y[2*stride_y] = center_y - height;

	x[3*stride_x] = center_x + width;
	y[3*stride_y] = center_y - height;
}
