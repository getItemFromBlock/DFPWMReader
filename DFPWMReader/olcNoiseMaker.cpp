#pragma once

#include "olcNoiseMaker.hpp"


olcNoiseMaker::olcNoiseMaker()
{
}

olcNoiseMaker::olcNoiseMaker(std::wstring sOutputDevice, unsigned int nSampleRate, unsigned int nChannels, unsigned int nBlocks, unsigned int nBlockSamples)
{
	Create(sOutputDevice, nSampleRate, nChannels, nBlocks, nBlockSamples);
}

olcNoiseMaker::~olcNoiseMaker()
{
	Destroy();
}

bool olcNoiseMaker::Create(std::wstring sOutputDevice, unsigned int nSampleRate, unsigned int nChannels, unsigned int nBlocks, unsigned int nBlockSamples)
{
	m_bReady = false;
	m_nSampleRate = nSampleRate;
	m_nChannels = nChannels;
	m_nBlockCount = nBlocks;
	m_nBlockSamples = nBlockSamples;
	m_nBlockFree = m_nBlockCount;
	m_nBlockCurrent = 0;
	m_pBlockMemory = nullptr;
	m_pWaveHeaders = nullptr;

	m_userFunction = nullptr;

	// Validate device
	std::vector<std::wstring> devices = Enumerate();
	auto d = std::find(devices.begin(), devices.end(), sOutputDevice);
	if (d != devices.end())
	{
		// Device is available
		int nDeviceID = (int)distance(devices.begin(), d);
		WAVEFORMATEX waveFormat;
		waveFormat.wFormatTag = WAVE_FORMAT_PCM;
		waveFormat.nSamplesPerSec = m_nSampleRate;
		waveFormat.wBitsPerSample = sizeof(short) * 8;
		waveFormat.nChannels = m_nChannels;
		waveFormat.nBlockAlign = (waveFormat.wBitsPerSample / 8) * waveFormat.nChannels;
		waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;
		waveFormat.cbSize = 0;

		// Open Device if valid
		if (waveOutOpen(&m_hwDevice, nDeviceID, &waveFormat, (DWORD_PTR)waveOutProcWrap, (DWORD_PTR)this, CALLBACK_FUNCTION) != S_OK)
		{
			Destroy();
			return false;
		}
	}

	// Allocate Wave|Block Memory
	m_pBlockMemory = new short[m_nBlockCount * m_nBlockSamples];
	if (m_pBlockMemory == nullptr)
	{
		Destroy();
		return false;
	}
	ZeroMemory(m_pBlockMemory, sizeof(short) * m_nBlockCount * m_nBlockSamples);

	m_pWaveHeaders = new WAVEHDR[m_nBlockCount];
	if (m_pWaveHeaders == nullptr)
	{
		Destroy();
		return false;
	}
	ZeroMemory(m_pWaveHeaders, sizeof(WAVEHDR) * m_nBlockCount);

	// Link headers to block memory
	for (unsigned int n = 0; n < m_nBlockCount; n++)
	{
		m_pWaveHeaders[n].dwBufferLength = m_nBlockSamples * sizeof(short);
		m_pWaveHeaders[n].lpData = (LPSTR)(m_pBlockMemory + (n * m_nBlockSamples));
	}

	m_bReady = true;

	m_thread = std::thread(&olcNoiseMaker::MainThread, this);

	// Start the ball rolling
	std::unique_lock<std::mutex> lm(m_muxBlockNotZero);
	m_cvBlockNotZero.notify_one();
	state = true;

	return true;
}

void olcNoiseMaker::Destroy()
{
	if (m_hwDevice)
	{
		waveOutReset(m_hwDevice);
		waveOutClose(m_hwDevice);
		m_hwDevice = NULL;
	}
	if (m_pBlockMemory)
	{
		delete[] m_pBlockMemory;
		m_pBlockMemory = nullptr;
	}
	if (m_pWaveHeaders)
	{
		delete[] m_pWaveHeaders;
		m_pWaveHeaders = nullptr;
	}
	state = false;
}

void olcNoiseMaker::Stop()
{
	m_bReady = false;
	m_thread.join();
}

std::vector<std::wstring> olcNoiseMaker::Enumerate()
{
	int nDeviceCount = waveOutGetNumDevs();
	std::vector<std::wstring> sDevices;
	WAVEOUTCAPS woc;
	for (int n = 0; n < nDeviceCount; n++)
		if (waveOutGetDevCapsW(n, &woc, sizeof(WAVEOUTCAPS)) == S_OK)
			sDevices.push_back(woc.szPname);
	return sDevices;
}

void olcNoiseMaker::SetUserFunction(void(*func)(short*, int, double))
{
	m_userFunction = func;
}

short olcNoiseMaker::clip(short dSample, short dMax)
{
	if (dSample >= 0)
		return dSample < dMax ? dSample : dMax;
	else
		return dSample > -dMax ? dSample : -dMax;
}

void olcNoiseMaker::waveOutProc(HWAVEOUT hWaveOut, UINT uMsg, DWORD dwParam1, DWORD dwParam2)
{
	if (uMsg != WOM_DONE) return;

	m_nBlockFree++;
	std::unique_lock<std::mutex> lm(m_muxBlockNotZero);
	m_cvBlockNotZero.notify_one();
}

void olcNoiseMaker::waveOutProcWrap(HWAVEOUT hWaveOut, UINT uMsg, DWORD_PTR dwInstance, DWORD dwParam1, DWORD dwParam2)
{
	((olcNoiseMaker*)dwInstance)->waveOutProc(hWaveOut, uMsg, dwParam1, dwParam2);
}

void olcNoiseMaker::MainThread()
{
	m_dGlobalTime = 0.0;
	float dTimeStep = 1.0f / (float)m_nSampleRate;

	while (m_bReady)
	{
		// Wait for block to become available
		if (m_nBlockFree == 0)
		{
			std::unique_lock<std::mutex> lm(m_muxBlockNotZero);
			while (m_nBlockFree == 0) // sometimes, Windows signals incorrectly
				m_cvBlockNotZero.wait(lm);
		}

		// Block is here, so use it
		m_nBlockFree--;

		// Prepare block for processing
		if (m_pWaveHeaders[m_nBlockCurrent].dwFlags & WHDR_PREPARED)
			waveOutUnprepareHeader(m_hwDevice, &m_pWaveHeaders[m_nBlockCurrent], sizeof(WAVEHDR));

		int nCurrentBlock = m_nBlockCurrent * m_nBlockSamples;

		m_userFunction(m_pBlockMemory + nCurrentBlock, m_nBlockSamples, m_dGlobalTime);
		
		m_dGlobalTime = m_dGlobalTime + dTimeStep * m_nBlockSamples;

		// Send block to sound device
		waveOutPrepareHeader(m_hwDevice, &m_pWaveHeaders[m_nBlockCurrent], sizeof(WAVEHDR));
		waveOutWrite(m_hwDevice, &m_pWaveHeaders[m_nBlockCurrent], sizeof(WAVEHDR));
		m_nBlockCurrent++;
		m_nBlockCurrent %= m_nBlockCount;
	}
}
