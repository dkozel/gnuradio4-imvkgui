/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of Annotation and AnnotationSet
 */
#ifndef Annotation_h
#define Annotation_h

#include <cstdint>
#include <string>
#include <vector>

/**
	@brief One labelled region of a recording, in display-neutral form

	Deliberately knows nothing about SigMF, flatbuffers, ImGui or scopehal. The conversion from
	the record lives in exactly one place - BuildAnnotationSet() in SigMFSource.cpp - so that
	the display layer includes this header and nothing else, and so that libsigmf's quirks stop
	at that boundary. See notes/annotation-overlay-plan.md §A1.

	The independent reason for the boundary is that libsigmf's own get_sigmf_in_range() cannot
	be used per frame: it returns a whole new record by value, re-sorts an already sorted
	vector on every call, rebases sample_start into segment coordinates, takes a non-const
	reference, and hangs outright on an annotation with no sample_start
	(sigmf_helpers.h:123-125). Plan §8.1 has the detail.
 */
struct Annotation
{
	///@brief Recording coordinates, always valid
	int64_t sampleStart = 0;

	///@brief Exclusive. Equal to sampleStart when core:sample_count is absent.
	int64_t sampleEnd = 0;

	///@brief Absolute frequency of the lower edge, Hz. Meaningless unless hasFreq.
	double freqLoHz = 0;

	///@brief Absolute frequency of the upper edge, Hz. Meaningless unless hasFreq.
	double freqHiHz = 0;

	/**
		@brief True if both frequency edges were present

		One edge alone is not a band, so it is treated as no band at all rather than as a
		half-open one. An annotation without a frequency extent still says something about a
		time range, and is drawn full width.
	 */
	bool hasFreq = false;

	std::string label;
	std::string description;
	std::string comment;
	std::string generator;

	/**
		@brief Hash of the label, resolved to a colour at draw time

		Stored rather than resolved here so that the model carries no colour policy: the same
		emitter keeps its colour in both panes because both call ColorForKey() on the same
		number, not because the model decided what red is.
	 */
	uint32_t colorKey = 0;
};

///@brief An RGB colour with components in [0, 1]. Packed into whatever the renderer wants.
struct AnnotationColor
{
	float r = 0;
	float g = 0;
	float b = 0;
};

/**
	@brief Reserved colour key for an annotation with no label

	FNV-1a of a non-empty string could in principle collide with this; the consequence is one
	unlabelled-looking colour on one label, which is not worth defending against.
 */
static const uint32_t g_unlabelledColorKey = 0;

/**
	@brief Resolves a colour key to a colour

	Hue steps by the golden ratio conjugate, which spreads successive keys as far apart on the
	wheel as a sequence can be, with fixed saturation and value so that no label is rendered
	much lighter or darker than another. g_unlabelledColorKey maps to a neutral grey.
 */
AnnotationColor ColorForKey(uint32_t colorKey);

///@brief FNV-1a over a label, giving the colour key. Empty maps to g_unlabelledColorKey.
uint32_t ColorKeyForLabel(const std::string& label);

/**
	@brief A sorted, queryable collection of annotations

	@par Why a running-maximum prefix and not an interval tree

	Sorting by start alone does not make an overlap query a clean binary search: a long
	annotation starting far before the window still overlaps it. The usual answers are an
	interval tree or a segment tree. For a set that is built once and never modified, a
	running maximum of the end over [0, i] gives the same asymptotics in a few lines:

	1. Binary search for the last annotation whose start is below the window's end.
	2. Walk backwards from there, emitting anything that reaches into the window, and stop as
	   soon as the running maximum drops to or below the window's start - because then nothing
	   at or before that index can reach the window either.

	O(log n + hits). On the 9182-annotation omnisig recording that is a handful of comparisons
	per query.
 */
class AnnotationSet
{
public:
	void Clear();

	///@brief Adds one annotation. Call Finalize() once when done.
	void Add(const Annotation& a);

	///@brief Sorts by sampleStart and builds the prefix array. Idempotent.
	void Finalize();

	size_t size() const
	{ return m_annotations.size(); }

	bool empty() const
	{ return m_annotations.empty(); }

	const Annotation& operator[](size_t i) const
	{ return m_annotations[i]; }

	/**
		@brief Indices of every annotation overlapping the half-open sample range [lo, hi)

		@param lo	Start of the window, inclusive
		@param hi	End of the window, exclusive
		@param out	Indices are appended; not cleared first, so several queries can accumulate

		A zero-length annotation - one whose core:sample_count was absent - counts as
		overlapping when its start lies within the window, so it still draws as a one pixel
		line rather than vanishing.
	 */
	void QueryOverlapping(int64_t lo, int64_t hi, std::vector<size_t>& out) const;

	/**
		@brief The end of an annotation for overlap purposes

		Zero-length annotations are widened to one sample here and nowhere else, so that the
		query test and the prefix array cannot disagree about whether one of them is inside a
		window.
	 */
	static int64_t EffectiveEnd(const Annotation& a)
	{ return (a.sampleEnd > a.sampleStart) ? a.sampleEnd : (a.sampleStart + 1); }

protected:
	///@brief Sorted by sampleStart once Finalize() has run
	std::vector<Annotation> m_annotations;

	///@brief Running maximum of EffectiveEnd() over [0, i]
	std::vector<int64_t> m_maxEndPrefix;

	///@brief False until Finalize() has run over the current contents
	bool m_finalized = false;
};

#endif
