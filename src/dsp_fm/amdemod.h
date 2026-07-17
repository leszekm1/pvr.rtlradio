#ifndef AMDEMOD_H
#define AMDEMOD_H

#include "datatypes.h"

class CAmDemod
{
public:
	CAmDemod(TYPEREAL samplerate);
	int ProcessData(int InLength, TYPECPX* pInData, TYPEREAL* pOutData);
	int ProcessData(int InLength, TYPECPX* pInData, TYPECPX* pOutData);
	void SetSampleRate(TYPEREAL samplerate);

private:
	TYPEREAL Demodulate(TYPECPX const& sample);

	TYPEREAL m_DcAlpha;
	TYPEREAL m_DcAverage;
	TYPEREAL m_LevelAlpha;
	TYPEREAL m_AudioLevel;
};

#endif // AMDEMOD_H
