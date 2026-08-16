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
	Complex FFT magnitude postprocess for a spectrum *trace*.

	This is scopeprotocols' ComplexToLogMagnitude.glsl plus the half-block fftshift
	rotation from ComplexSpectrogramPostprocess.glsl, and nothing else. Neither upstream
	shader can be used as-is:

	  - ComplexToLogMagnitude.glsl produces exactly the dBm value we want but reads
	    din[i*2] with no rotation, so the output runs DC..+fs/2..-fs/2..DC. Correct for a
	    real FFT (where there are no negative bins); wrong for a complex one.

	  - ComplexSpectrogramPostprocess.glsl has the rotation, but fuses in a 0-1
	    normalization against Range Min/Max and writes a transposed, column-major
	    spectrogram image (nout = x*nblocks + y). We need unnormalized dBm in a linear
	    trace layout.

	Batched: the input holds nblocks consecutive transforms of npoints complex bins each,
	contiguous and unpadded, which is the layout a VkFFT plan with numberBatches > 1 writes
	(VulkanFFTPlan.cpp:89-95). The output holds the same number of spectra of npoints real
	dBm values. One dispatch covers the whole block; with nblocks == 1 this is exactly the
	single-spectrum shader it grew from.

	See DESIGN.md sections 7.2 and 13.
 */

#version 430
#pragma shader_stage(compute)

layout(std430, binding=0) restrict readonly buffer buf_din
{
	float din[];
};

layout(std430, binding=1) restrict writeonly buffer buf_dout
{
	float dout[];
};

layout(std430, push_constant) uniform constants
{
	//Number of FFT bins per transform, which for a complex transform equals the number of
	//input points
	uint npoints;

	//Number of consecutive transforms in the buffer
	uint nblocks;

	//Power scale: (volts per unit of bin magnitude)^2 / impedance
	float scale;
};

layout(local_size_x=64, local_size_y=1, local_size_z=1) in;

void main()
{
	uint i = (gl_GlobalInvocationID.y * gl_NumWorkGroups.x * gl_WorkGroupSize.x) + gl_GlobalInvocationID.x;

	//If off end of array, stop
	if(i >= npoints * nblocks)
		return;

	//Which spectrum, and which bin within it
	uint block = i / npoints;
	uint bin = i - (block * npoints);

	//fftshift: output bin b is at frequency (b - npoints/2) bins from center, which lives
	//in FFT bin (b + npoints/2) mod npoints. Output bin npoints/2 is therefore DC.
	uint isample = bin + (npoints/2);
	if(isample >= npoints)
		isample -= npoints;

	uint inbase = (block * npoints + isample) * 2;
	float real = din[inbase];
	float imag = din[inbase + 1];

	float v = real*real + imag*imag;

	//dBW to dBm is +30. No normalization: this is an absolute-calibrated trace.
	dout[i] = (10 * log(v * scale) / log(10)) + 30;
}
