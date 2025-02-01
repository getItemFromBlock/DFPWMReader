#pragma once

#pragma comment(lib, "winmm.lib")

#include <iostream>
#include <cmath>
#include <fstream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <condition_variable>

#include <Windows.h>

class olcNoiseMaker
{
public:
	olcNoiseMaker();

	olcNoiseMaker(std::wstring sOutputDevice, unsigned int nSampleRate = 44100, unsigned int nChannels = 1, unsigned int nBlocks = 8, unsigned int nBlockSamples = 512);

	~olcNoiseMaker();

	bool Create(std::wstring sOutputDevice, unsigned int nSampleRate = 44100, unsigned int nChannels = 1, unsigned int nBlocks = 8, unsigned int nBlockSamples = 512);

	void Destroy();

	void Stop();

	double GetTime() const {	return m_dGlobalTime; }

	bool GetState() const {	return state;}

public:
	static std::vector<std::wstring> Enumerate();

	void SetUserFunction(void(*func)(short*, int, double));

	short clip(short dSample, short dMax);


private:
	void(*m_userFunction)(short*, int, double);

	unsigned int m_nSampleRate;
	unsigned int m_nChannels;
	unsigned int m_nBlockCount;
	unsigned int m_nBlockSamples;
	unsigned int m_nBlockCurrent;

	short* m_pBlockMemory;
	WAVEHDR* m_pWaveHeaders;
	HWAVEOUT m_hwDevice = NULL;

	std::thread m_thread;
	std::atomic<bool> m_bReady;
	std::atomic<unsigned int> m_nBlockFree;
	std::condition_variable m_cvBlockNotZero;
	std::mutex m_muxBlockNotZero;

	std::atomic<double> m_dGlobalTime;

	bool state = false;

	// Handler for soundcard request for more data
	void waveOutProc(HWAVEOUT hWaveOut, UINT uMsg, DWORD dwParam1, DWORD dwParam2);

	// Static wrapper for sound card handler
	static void CALLBACK waveOutProcWrap(HWAVEOUT hWaveOut, UINT uMsg, DWORD_PTR dwInstance, DWORD dwParam1, DWORD dwParam2);

	// Main thread. This loop responds to requests from the soundcard to fill 'blocks'
	// with audio data. If no requests are available it goes dormant until the sound
	// card is ready for more data. The block is fille by the "user" in some manner
	// and then issued to the soundcard.
	void MainThread();
};