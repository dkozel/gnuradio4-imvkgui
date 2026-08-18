/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Implementation of AnnotationSet and the label colouring
 */

#include "Annotation.h"

#include <algorithm>
#include <cmath>

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Colour

uint32_t ColorKeyForLabel(const string& label)
{
	if(label.empty())
		return g_unlabelledColorKey;

	//FNV-1a, 32 bit. Chosen for being four lines and well mixed in the low bits, which is
	//where the hue comes from.
	uint32_t hash = 2166136261u;
	for(size_t i=0; i<label.size(); i++)
	{
		hash ^= static_cast<uint8_t>(label[i]);
		hash *= 16777619u;
	}

	//Never collide with the reserved key: an unlabelled annotation must be distinguishable
	//from a labelled one that happened to hash to zero
	if(hash == g_unlabelledColorKey)
		hash = 1;

	return hash;
}

AnnotationColor ColorForKey(uint32_t colorKey)
{
	AnnotationColor ret;

	//No label, no identity to colour code. Neutral grey says "something is here" without
	//implying it belongs to the same emitter as anything else grey.
	if(colorKey == g_unlabelledColorKey)
	{
		ret.r = 0.72f;
		ret.g = 0.72f;
		ret.b = 0.75f;
		return ret;
	}

	//Golden ratio conjugate stepping. Multiplying by an irrational and taking the fractional
	//part spreads successive values about as far apart on the wheel as a sequence can be, so
	//two labels that hash to nearby numbers still get well separated hues.
	const double phi = 0.618033988749895;
	double h = fmod(static_cast<double>(colorKey >> 8) * phi, 1.0);

	//Fixed saturation and value, so no label comes out much lighter or darker than another and
	//all of them read against the density map behind
	const double s = 0.65;
	const double v = 0.95;

	double hs = h * 6.0;
	int sector = static_cast<int>(hs) % 6;
	double f = hs - floor(hs);
	double p = v * (1.0 - s);
	double q = v * (1.0 - s * f);
	double t = v * (1.0 - s * (1.0 - f));

	double r = 0;
	double g = 0;
	double b = 0;
	switch(sector)
	{
		case 0:  r = v; g = t; b = p; break;
		case 1:  r = q; g = v; b = p; break;
		case 2:  r = p; g = v; b = t; break;
		case 3:  r = p; g = q; b = v; break;
		case 4:  r = t; g = p; b = v; break;
		default: r = v; g = p; b = q; break;
	}

	ret.r = static_cast<float>(r);
	ret.g = static_cast<float>(g);
	ret.b = static_cast<float>(b);
	return ret;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// AnnotationSet

void AnnotationSet::Clear()
{
	m_annotations.clear();
	m_maxEndPrefix.clear();
	m_finalized = false;
}

void AnnotationSet::Add(const Annotation& a)
{
	m_annotations.push_back(a);
	m_finalized = false;
}

void AnnotationSet::Finalize()
{
	if(m_finalized)
		return;

	sort(m_annotations.begin(), m_annotations.end(),
		[](const Annotation& a, const Annotation& b)
		{ return a.sampleStart < b.sampleStart; });

	m_maxEndPrefix.resize(m_annotations.size());
	int64_t running = INT64_MIN;
	for(size_t i=0; i<m_annotations.size(); i++)
	{
		int64_t end = EffectiveEnd(m_annotations[i]);
		if(end > running)
			running = end;
		m_maxEndPrefix[i] = running;
	}

	m_finalized = true;
}

void AnnotationSet::QueryOverlapping(int64_t lo, int64_t hi, vector<size_t>& out) const
{
	if(m_annotations.empty() || (hi <= lo))
		return;

	//Last annotation whose start is strictly below the end of the window. Anything past this
	//starts after the window and cannot overlap it.
	//
	//Comparing against a bare int64 rather than a dummy Annotation so that the comparator has
	//nothing to disagree with the sort about.
	size_t last = static_cast<size_t>(
		upper_bound(m_annotations.begin(), m_annotations.end(), hi - 1,
			[](int64_t value, const Annotation& a)
			{ return value < a.sampleStart; })
		- m_annotations.begin());
	if(last == 0)
		return;

	//Walk backwards. The prefix is a running maximum, so once it drops to or below the start
	//of the window, nothing at or before this index reaches into the window either and the
	//scan is done - this is what keeps the cost proportional to hits rather than to set size.
	for(size_t i=last; i-- > 0; )
	{
		if(m_maxEndPrefix[i] <= lo)
			break;

		if(EffectiveEnd(m_annotations[i]) > lo)
			out.push_back(i);
	}
}
