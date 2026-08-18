/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
* Copyright (c) 2026 Derek Kozel and contributors                                                                      *
* All rights reserved.                                                                                                 *
*                                                                                                                      *
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the     *
* following conditions are met:                                                                                        *
*                                                                                                                      *
*    * Redistributions of source code must retain the above copyright notice, this list of conditions, and the         *
*      following disclaimer.                                                                                           *
*                                                                                                                      *
*    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the       *
*      following disclaimer in the documentation and/or other materials provided with the distribution.                *
*                                                                                                                      *
*    * Neither the name of the author nor the names of any contributors may be used to endorse or promote products     *
*      derived from this software without specific prior written permission.                                           *
*                                                                                                                      *
* THIS SOFTWARE IS PROVIDED BY THE AUTHORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   *
* TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL *
* THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES        *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR       *
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE       *
* POSSIBILITY OF SUCH DAMAGE.                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/*
	@brief Unpacks raw interleaved I/Q samples, converts them to float, windows them, and
	writes the interleaved complex buffer VkFFT consumes - all in one pass.

	This replaces the whole of scopeprotocols' Complex*Window.glsl family for any input that
	arrives in its on-disk binary format. Three things are fused here that used to be
	separate:

	  1. Integer to float conversion, which used to be a scalar CPU loop in
	     SigMFSource::ConvertSamples() and was 22.7% of playback wall clock (DESIGN.md
	     section 13).
	  2. Deinterleaving into planar I and Q, which used to happen on the CPU in that same
	     loop, only for the window shader to interleave them straight back again.
	  3. The window function itself.

	There is no deinterleave step here, in either direction. Both components of a complex
	sample arrive in the same register from the same load for the 8- and 16-bit formats, and
	the output index is the input index, so the layout the file already had is the layout
	VkFFT wants.

	@par No extension requirements

	Components narrower than 32 bits are extracted from uint words with bitfieldExtract
	rather than declared as int16_t/int8_t. That would need VK_KHR_16bit_storage or
	VK_KHR_8bit_storage plus shaderInt16/shaderInt8, which scopehal probes for
	(VulkanInit.cpp:817, :847) but which are not universal - and it would buy nothing, since
	bitfieldExtract is a single instruction either way. std430 uint[] works on any Vulkan 1.0
	device.

	@par Endianness

	Words are loaded with the device's native byte order, so this handles a file whose byte
	order matches the host. SigMFSource keeps the CPU path for anything else rather than
	byteswapping here; SigMF is little-endian in practice and a big-endian recording is not
	worth a second set of unpack paths.

	@par Dispatch shape

	One dispatch covers every transform in the block. x and y index the sample within a
	transform, exactly as the shaders this replaces used them, and z indexes which transform.
	The predecessor could not do this - it derived the window coefficient from the raw global
	thread index, so a thread in the second transform would have got the wrong coefficient -
	and it therefore needed one dispatch per transform, 128 of them per block at 1 Msample
	and 8192 points. Splitting the two indices apart makes the whole batch one dispatch and
	costs nothing: the window coefficient comes from the x/y index, which is already the
	within-transform position.
 */

#version 430
#pragma shader_stage(compute)

layout(std430, binding=0) restrict readonly buffer buf_din
{
	//Raw file bytes, reinterpreted as words. Never indexed as samples; see UnpackSample().
	uint din[];
};

layout(std430, binding=1) restrict writeonly buffer buf_dout
{
	//Interleaved complex, real at even indices and imaginary at odd
	float dout[];
};

layout(std430, push_constant) uniform constants
{
	///@brief Transform length, in complex samples
	uint npoints;

	///@brief Complex samples actually present in din. Threads past this zero fill.
	uint nsamples;

	///@brief One of the FORMAT_* values below
	uint format;

	///@brief Added to each raw component before scaling. Nonzero only for offset binary formats.
	float sampleBias;

	///@brief Multiplied into each biased component to reach volts
	float sampleScale;

	///@brief 2*pi/npoints, the window's phase advance per sample
	float phaseStep;

	//Generalized cosine-sum window coefficients:
	//  w = a0 - a1*cos(x) + a2*cos(2x) - a3*cos(3x)
	//which covers every window this filter offers. See ComplexFFTFilter::Refresh() for the
	//values, and for why Blackman-Harris uses cos(3x) here rather than the cos(6x) that
	//scopeprotocols' ComplexBlackmanHarrisWindow.glsl:94 has.
	float alpha0;
	float alpha1;
	float alpha2;
	float alpha3;
};

//Must match PackedIQFormat in PackedIQWaveform.h
const uint FORMAT_INT8		= 0;
const uint FORMAT_UINT8		= 1;
const uint FORMAT_INT16		= 2;
const uint FORMAT_UINT16	= 3;
const uint FORMAT_INT32		= 4;
const uint FORMAT_UINT32	= 5;
const uint FORMAT_FLOAT32	= 6;

layout(local_size_x=64, local_size_y=1, local_size_z=1) in;

/**
	@brief Reads complex sample i out of the packed buffer and returns it as raw component values

	Scaling is deliberately not applied here: the caller folds it into the window multiply,
	so the conversion costs no extra arithmetic at all.

	The branch is on a push constant, so it is uniform across the entire dispatch and every
	invocation takes the same side of it. There is no divergence to pay for.
 */
vec2 UnpackSample(uint i)
{
	//Two components per word. Both halves of a complex sample live in the same word, so one
	//load serves both - this is the case the whole datapath exists for.
	if( (format == FORMAT_INT16) || (format == FORMAT_UINT16) )
	{
		uint w = din[i];
		if(format == FORMAT_INT16)
			return vec2(bitfieldExtract(int(w), 0, 16), bitfieldExtract(int(w), 16, 16));
		else
			return vec2(bitfieldExtract(w, 0, 16), bitfieldExtract(w, 16, 16));
	}

	//Four components per word, so one word holds two complex samples. Sample i occupies the
	//low half of its word when i is even and the high half when it is odd.
	else if( (format == FORMAT_INT8) || (format == FORMAT_UINT8) )
	{
		uint w = din[i >> 1];
		int shift = int((i & 1) * 16);
		if(format == FORMAT_INT8)
			return vec2(bitfieldExtract(int(w), shift, 8), bitfieldExtract(int(w), shift + 8, 8));
		else
			return vec2(bitfieldExtract(w, shift, 8), bitfieldExtract(w, shift + 8, 8));
	}

	//One component per word: two adjacent words, still one coalesced 8-byte access per thread.
	//
	//For FORMAT_UINT32 the widening to float happens before sampleBias is applied, so a value
	//near full scale loses about one bit more than the CPU path's exact 64-bit subtract would.
	//The destination is a 32-bit float either way, so both paths discard the low 8 bits of a
	//32-bit sample regardless; the 8- and 16-bit formats are exact.
	else
	{
		uint wr = din[i*2 + 0];
		uint wi = din[i*2 + 1];
		if(format == FORMAT_FLOAT32)
			return vec2(uintBitsToFloat(wr), uintBitsToFloat(wi));
		else if(format == FORMAT_INT32)
			return vec2(int(wr), int(wi));
		else
			return vec2(wr, wi);
	}
}

void main()
{
	//Position within one transform. x and y are split this way, rather than x alone, because
	//a transform longer than 32768 workgroups overflows the x dimension; same scheme the
	//shaders this replaces used.
	uint n = (gl_GlobalInvocationID.y * gl_NumWorkGroups.x * gl_WorkGroupSize.x) + gl_GlobalInvocationID.x;

	//Which transform in the batch
	uint blk = gl_GlobalInvocationID.z;

	//Off the end of this transform: the dispatch is rounded up to a whole workgroup
	if(n >= npoints)
		return;

	//Batches are contiguous and unpadded, matching m_rdinbuf's layout and VkFFT's
	//expectations (VulkanFFTPlan.cpp:89-95)
	uint i = blk * npoints + n;
	uint outbase = i * 2;

	//Off the end of the input. ComplexFFTFilter only ever dispatches whole transforms that
	//fit, so this is unreachable today; it is here so that a short final block degrades to a
	//zero-padded transform rather than reading out of bounds.
	if(i >= nsamples)
	{
		dout[outbase + 0] = 0;
		dout[outbase + 1] = 0;
		return;
	}

	vec2 iq = UnpackSample(i);

	//Generalized cosine sum. Rectangular reduces to a0=1 with the rest zero, so it costs
	//three cosines it does not need - which is what the predecessor's separate rectangular
	//shader avoided. Not worth three pipelines: this shader is memory bound at every sample
	//rate the application targets, and the transcendentals hide under the load latency.
	float num = float(n) * phaseStep;
	float w =
		alpha0 -
		alpha1 * cos(num) +
		alpha2 * cos(2.0 * num) -
		alpha3 * cos(3.0 * num);

	//The window multiply and the format scaling collapse into one multiply per component,
	//which is why UnpackSample() returns raw values
	float wscale = w * sampleScale;
	dout[outbase + 0] = (iq.x + sampleBias) * wscale;
	dout[outbase + 1] = (iq.y + sampleBias) * wscale;
}
