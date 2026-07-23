// Copyright (c) 2013- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <set>

#include "Common/UI/UIScreen.h"
#include "Common/UI/ViewGroup.h"
#include "Common/File/Path.h"
#include "UI/BaseScreens.h"

struct MusicTrack {
	Path path;
	std::string title;
	std::string format; // "FLAC" or "MP3" or "WAV"
	int durationSec = 0;
	bool isFlac = false;
	bool isMp3 = false;
	bool isWav = false;
};

class MusicPlayerScreen : public UIBaseScreen {
public:
	MusicPlayerScreen();
	~MusicPlayerScreen();

	const char *tag() const override { return "MusicPlayer"; }

	void update() override;
	void CreateViews() override;

protected:
	void DrawBackground(UIContext &dc) override;

private:
	void ScanMusicFiles();
	void ScanDirectoryRecursively(const Path &dir, int depth, std::set<std::string> &uniquePaths, int maxTracks);
	void StartPlayback(int trackIdx);
	void PlayThreadFunc();
	void StopPlayback();

	// Event Handlers
	void OnTrackClick(UI::EventParams &e);
	void OnPlayPauseClick(UI::EventParams &e);
	void OnStopClick(UI::EventParams &e);
	void OnNextClick(UI::EventParams &e);
	void OnPrevClick(UI::EventParams &e);

	std::vector<MusicTrack> tracks_;
	int currentTrackIdx_ = -1;
	std::atomic<bool> isPlaying_{false};
	std::atomic<bool> stopThread_{false};

	std::thread playThread_;
	std::mutex playMutex_;

	// For animated wave visualizer
	std::atomic<float> visualizerAmplitude_{0.0f};
	double lastVisualizerUpdate_ = 0.0;
};
