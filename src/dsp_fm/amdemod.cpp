#include "amdemod.h"

#include <algorithm>

CAmDemod::CAmDemod(TYPEREAL samplerate)
  : m_DcAlpha(0), m_DcAverage(0), m_LevelAlpha(0), m_AudioLevel(1)
{
	SetSampleRate(samplerate);
}

void CAmDemod::SetSampleRate(TYPEREAL samplerate)
{
	// Remove the carrier and very-low-frequency drift while preserving speech.
	m_DcAlpha = 1.0 - MEXP(-K_2PI * 30.0 / samplerate);
	// A slow audio-level follower keeps weak and strong stations usable.
	m_LevelAlpha = 1.0 - MEXP(-1.0 / (samplerate * 0.25));
}

TYPEREAL CAmDemod::Demodulate(TYPECPX const& sample)
{
	TYPEREAL const envelope = MSQRT((sample.re * sample.re) + (sample.im * sample.im));
	m_DcAverage += m_DcAlpha * (envelope - m_DcAverage);
	TYPEREAL const audio = envelope - m_DcAverage;
	m_AudioLevel += m_LevelAlpha * (MFABS(audio) - m_AudioLevel);
	TYPEREAL const gain = std::max<TYPEREAL>(0.25, std::min<TYPEREAL>(16.0, 6000.0 / std::max<TYPEREAL>(m_AudioLevel, 1.0)));
	return audio * gain;
}

int CAmDemod::ProcessData(int InLength, TYPECPX* pInData, TYPEREAL* pOutData)
{
	for(int i = 0; i < InLength; ++i)
		pOutData[i] = Demodulate(pInData[i]);
	return InLength;
}

int CAmDemod::ProcessData(int InLength, TYPECPX* pInData, TYPECPX* pOutData)
{
	for(int i = 0; i < InLength; ++i)
	{
		TYPEREAL const audio = Demodulate(pInData[i]);
		pOutData[i].re = audio;
		pOutData[i].im = audio;
	}
	return InLength;
}
