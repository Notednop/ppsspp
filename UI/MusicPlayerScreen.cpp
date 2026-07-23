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

#include <algorithm>
#include <cmath>

#define DRFLAC_API static
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "ext/libchdr/include/dr_libs/dr_flac.h"
#include "ext/minimp3/minimp3_ex.h"

#include "Common/File/FileUtil.h"
#include "Common/File/DirListing.h"
#include "Common/UI/Root.h"
#include "Common/UI/Context.h"
#include "Common/UI/View.h"
#include "Common/UI/ViewGroup.h"
#include "Common/UI/ScrollView.h"
#include "Common/Render/DrawBuffer.h"
#include "Common/Data/Color/RGBAUtil.h"
#include "Common/Data/Text/I18n.h"
#include "Common/System/System.h"
#include "Common/System/Display.h"
#include "Common/TimeUtil.h"
#include "Core/Config.h"
#include "Core/Util/PathUtil.h"
#include "UI/MusicPlayerScreen.h"

// Simple linear resampler to 44100Hz Stereo
static void ResampleTo44100Stereo(const int16_t* inSamples, int inChannels, int inRate, int inFrames, std::vector<int16_t>& outSamples) {
	if (inFrames <= 0) return;

	double ratio = 44100.0 / (double)inRate;
	int outFrames = (int)(inFrames * ratio);
	outSamples.resize(outFrames * 2);

	for (int i = 0; i < outFrames; i++) {
		double inFrameIndex = i / ratio;
		int frame0 = (int)floor(inFrameIndex);
		int frame1 = std::min(frame0 + 1, inFrames - 1);
		double frac = inFrameIndex - frame0;

		int16_t left0, right0, left1, right1;
		if (inChannels == 2) {
			left0 = inSamples[frame0 * 2];
			right0 = inSamples[frame0 * 2 + 1];
			left1 = inSamples[frame1 * 2];
			right1 = inSamples[frame1 * 2 + 1];
		} else {
			left0 = right0 = inSamples[frame0];
			left1 = right1 = inSamples[frame1];
		}

		int16_t left = (int16_t)(left0 * (1.0 - frac) + left1 * frac);
		int16_t right = (int16_t)(right0 * (1.0 - frac) + right1 * frac);

		outSamples[i * 2] = left;
		outSamples[i * 2 + 1] = right;
	}
}

static std::vector<uint8_t> ReadFileBytes(const Path &path) {
	std::vector<uint8_t> buffer;
	FILE* f = File::OpenCFile(path, "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		size_t size = ftell(f);
		fseek(f, 0, SEEK_SET);
		buffer.resize(size);
		size_t read_bytes = fread(buffer.data(), 1, size, f);
		(void)read_bytes;
		fclose(f);
	}
	return buffer;
}

MusicPlayerScreen::MusicPlayerScreen() {
	ScanMusicFiles();
}

MusicPlayerScreen::~MusicPlayerScreen() {
	StopPlayback();
}

void MusicPlayerScreen::ScanMusicFiles() {
	tracks_.clear();

	std::vector<Path> roots;
	roots.push_back(GetSysDirectory(DIRECTORY_MEMSTICK_ROOT));

	// On Android, scan common external storage directories
#ifdef ANDROID
	roots.push_back(Path("/sdcard/Music"));
	roots.push_back(Path("/sdcard/Download"));
	roots.push_back(Path("/sdcard/PSP/MUSIC"));
#else
	// On desktop, scan the user home directory if we can find it
	const char* home = getenv("USERPROFILE"); // Windows
	if (!home) home = getenv("HOME"); // Linux/macOS
	if (home) {
		Path homePath(home);
		roots.push_back(homePath / "Music");
		roots.push_back(homePath / "Downloads");
	}
#endif

	// Also scan the last browsed directory
	if (!g_Config.currentDirectory.empty()) {
		roots.push_back(Path(g_Config.currentDirectory));
	}

	std::set<std::string> uniquePaths;
	int maxTracks = 500; // prevent UI overflow/excessive memory usage

	for (const auto& root : roots) {
		if (tracks_.size() >= (size_t)maxTracks) break;
		ScanDirectoryRecursively(root, 0, uniquePaths, maxTracks);
	}
}

void MusicPlayerScreen::ScanDirectoryRecursively(const Path &dir, int depth, std::set<std::string> &uniquePaths, int maxTracks) {
	if (depth > 4 || tracks_.size() >= (size_t)maxTracks) return;

	std::vector<File::FileInfo> files;
	if (!File::GetFilesInDir(dir, &files)) return;

	for (const auto &file : files) {
		if (tracks_.size() >= (size_t)maxTracks) break;

		if (file.isDirectory) {
			// Skip special hidden/system/parent directories to avoid infinite loops and lag
			if (file.name == "." || file.name == ".." || file.name.empty() || file.name.front() == '.') continue;
			ScanDirectoryRecursively(file.fullName, depth + 1, uniquePaths, maxTracks);
		} else {
			std::string ext = file.fullName.GetFileExtension();
			std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

			if (ext == ".flac" || ext == ".mp3" || ext == ".wav") {
				std::string pathStr = file.fullName.ToString();
				if (uniquePaths.count(pathStr)) continue;
				uniquePaths.insert(pathStr);

				MusicTrack track;
				track.path = file.fullName;
				track.title = file.name;
				if (ext == ".flac") {
					track.format = "FLAC";
					track.isFlac = true;
				} else if (ext == ".mp3") {
					track.format = "MP3";
					track.isMp3 = true;
				} else {
					track.format = "WAV";
					track.isWav = true;
				}
				tracks_.push_back(track);
			}
		}
	}
}

void MusicPlayerScreen::update() {
	UIBaseScreen::update();

	// Smooth decay of visualizer amplitude over time
	double now = time_now_d();
	double dt = now - lastVisualizerUpdate_;
	if (dt > 0.0) {
		visualizerAmplitude_ = std::max(0.0f, visualizerAmplitude_.load() - (float)(dt * 4.0f));
		lastVisualizerUpdate_ = now;
	}
}

void MusicPlayerScreen::StartPlayback(int trackIdx) {
	StopPlayback();

	if (trackIdx < 0 || trackIdx >= (int)tracks_.size()) return;

	currentTrackIdx_ = trackIdx;
	isPlaying_ = true;
	stopThread_ = false;

	playThread_ = std::thread(&MusicPlayerScreen::PlayThreadFunc, this);
}

void MusicPlayerScreen::StopPlayback() {
	isPlaying_ = false;
	stopThread_ = true;
	if (playThread_.joinable()) {
		playThread_.join();
	}
}

void MusicPlayerScreen::PlayThreadFunc() {
	if (currentTrackIdx_ < 0 || currentTrackIdx_ >= (int)tracks_.size()) return;

	const auto &track = tracks_[currentTrackIdx_];
	std::vector<uint8_t> fileData = ReadFileBytes(track.path);
	if (fileData.empty()) return;

	std::vector<int16_t> pcmSamples;

	if (track.isFlac) {
		drflac* pFlac = drflac_open_memory(fileData.data(), fileData.size(), nullptr);
		if (pFlac) {
			std::vector<int16_t> tempSamples((size_t)(pFlac->totalPCMFrameCount * pFlac->channels));
			drflac_read_pcm_frames_s16(pFlac, pFlac->totalPCMFrameCount, tempSamples.data());
			ResampleTo44100Stereo(tempSamples.data(), pFlac->channels, pFlac->sampleRate, pFlac->totalPCMFrameCount, pcmSamples);
			drflac_close(pFlac);
		}
	} else if (track.isMp3) {
		mp3dec_t mp3d;
		mp3dec_init(&mp3d);
		mp3dec_file_info_t info;
		int retval = mp3dec_load_buf(&mp3d, fileData.data(), fileData.size(), &info, nullptr, nullptr);
		if (retval >= 0 && info.samples > 0) {
			int inFrames = (int)(info.samples / info.channels);
			ResampleTo44100Stereo(info.buffer, info.channels, info.hz, inFrames, pcmSamples);
			free(info.buffer);
		}
	} else if (track.isWav) {
		// Basic WAV parser
		if (fileData.size() > 44) {
			int channels = *(int16_t*)&fileData[22];
			int sampleRate = *(int32_t*)&fileData[24];
			int bitsPerSample = *(int16_t*)&fileData[34];
			int dataSize = *(int32_t*)&fileData[40];
			int rawOffset = 44;

			if (bitsPerSample == 16 && dataSize > 0 && rawOffset + dataSize <= (int)fileData.size()) {
				int16_t* pRaw = (int16_t*)(fileData.data() + rawOffset);
				int inFrames = dataSize / (channels * 2);
				ResampleTo44100Stereo(pRaw, channels, sampleRate, inFrames, pcmSamples);
			}
		}
	}

	if (pcmSamples.empty()) return;

	size_t currentOffset = 0;
	size_t totalSamples = pcmSamples.size() / 2; // stereo frames
	const size_t chunkSize = 735; // ~16.6ms at 44100Hz

	std::vector<int32_t> mixBuffer(chunkSize * 2);

	while (!stopThread_ && isPlaying_) {
		if (currentOffset >= totalSamples) {
			// Auto loop track
			currentOffset = 0;
		}

		size_t framesToRead = std::min(chunkSize, totalSamples - currentOffset);
		if (framesToRead == 0) break;

		float ampSum = 0.0f;
		for (size_t i = 0; i < framesToRead; i++) {
			int16_t left = pcmSamples[(currentOffset + i) * 2];
			int16_t right = pcmSamples[(currentOffset + i) * 2 + 1];

			mixBuffer[i * 2] = (int32_t)left;
			mixBuffer[i * 2 + 1] = (int32_t)right;

			ampSum += std::abs(left) / 32768.0f;
		}

		// Update real-time visualization amplitude safely
		visualizerAmplitude_ = std::max(visualizerAmplitude_.load(), ampSum / (float)framesToRead);

		// Push to standard audio output
		System_AudioPushSamples(mixBuffer.data(), (int)framesToRead, 1.0f);

		currentOffset += framesToRead;

		// Feed in tight loop with small delay
		sleep_ms(16, "music_player_feed");
	}
}

void MusicPlayerScreen::CreateViews() {
	using namespace UI;

	auto mm = GetI18NCategory(I18NCat::MAINMENU);
	auto di = GetI18NCategory(I18NCat::DIALOG);

	// Elegant PlayStation 5 Glassmorphism UI
	// Left column: Scrollable list of tracks
	LinearLayout *rootLayout = new LinearLayout(ORIENT_HORIZONTAL, new LinearLayoutParams(FILL_PARENT, FILL_PARENT));

	LinearLayout *leftColumn = rootLayout->Add(new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(400, FILL_PARENT, Margins(16, 16, 16, 16))));
	leftColumn->Add(new TextView("MUSIC TRACKS", ALIGN_LEFT, false, new LinearLayoutParams(Margins(0, 0, 0, 16))));

	ScrollView *scrollTracks = leftColumn->Add(new ScrollView(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, 1.0f)));
	LinearLayout *trackList = scrollTracks->Add(new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT)));

	if (tracks_.empty()) {
		trackList->Add(new TextView("No tracks found under PSP/MUSIC", ALIGN_LEFT, true));
	} else {
		for (int i = 0; i < (int)tracks_.size(); i++) {
			std::string label = tracks_[i].title + " [" + tracks_[i].format + "]";
			Choice *trackChoice = trackList->Add(new Choice(label, ImageID("I_PLAY")));
			trackChoice->OnClick.Add([this, i](UI::EventParams &) {
				StartPlayback(i);
				RecreateViews();
			});
		}
	}

	// Right column: Beautiful animated cover disk and visual controls
	LinearLayout *rightColumn = rootLayout->Add(new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, FILL_PARENT, Margins(16, 16, 16, 16))));

	std::string nowPlaying = "Select a track to play";
	if (currentTrackIdx_ >= 0 && currentTrackIdx_ < (int)tracks_.size()) {
		nowPlaying = tracks_[currentTrackIdx_].title;
	}
	rightColumn->Add(new TextView("NOW PLAYING", ALIGN_HCENTER, true, new LinearLayoutParams(Margins(0, 32, 0, 8))));
	rightColumn->Add(new TextView(nowPlaying, ALIGN_HCENTER, false, new LinearLayoutParams(Margins(0, 0, 0, 32))));

	// Aesthetic rotating visualizer spacer (drawn in DrawBackground)
	rightColumn->Add(new Spacer(new LinearLayoutParams(FILL_PARENT, 200)));

	// Beautiful interactive playback controls
	LinearLayout *controlsRow = rightColumn->Add(new LinearLayout(ORIENT_HORIZONTAL, new LinearLayoutParams(WRAP_CONTENT, WRAP_CONTENT, 0.0f, Gravity::G_HCENTER)));
	controlsRow->SetSpacing(16.0f);

	Choice *prevBtn = controlsRow->Add(new Choice(ImageID("I_ARROW_LEFT")));
	prevBtn->OnClick.Handle(this, &MusicPlayerScreen::OnPrevClick);

	Choice *playPauseBtn = controlsRow->Add(new Choice(isPlaying_ ? ImageID("I_PAUSE") : ImageID("I_PLAY")));
	playPauseBtn->OnClick.Handle(this, &MusicPlayerScreen::OnPlayPauseClick);

	Choice *stopBtn = controlsRow->Add(new Choice(ImageID("I_STOP")));
	stopBtn->OnClick.Handle(this, &MusicPlayerScreen::OnStopClick);

	Choice *nextBtn = controlsRow->Add(new Choice(ImageID("I_ARROW_RIGHT")));
	nextBtn->OnClick.Handle(this, &MusicPlayerScreen::OnNextClick);

	rightColumn->Add(new Spacer(new LinearLayoutParams(1.0f)));

	Choice *backBtn = rightColumn->Add(new Choice(di->T("Back"), ImageID("I_NAVIGATE_BACK"), new LinearLayoutParams(180, WRAP_CONTENT, 0.0f, Gravity::G_HCENTER)));
	backBtn->OnClick.Handle<UIScreen>(this, &UIScreen::OnBack);

	root_ = rootLayout;
}

void MusicPlayerScreen::DrawBackground(UIContext &dc) {
	// Call base to draw the beautiful slow wave background
	UIBaseScreen::DrawBackground(dc);

	// Render the dynamic PS5-inspired interactive floating sound visualizer bars!
	Bounds bounds = dc.GetBounds();
	float centerX = bounds.centerX() + 200.0f;
	float centerY = bounds.centerY() - 32.0f;

	dc.Flush();
	dc.BeginNoTex();

	// Render glowing concentric visualizer soundwaves
	const int numBars = 16;
	float baseRadius = 60.0f + visualizerAmplitude_ * 40.0f;
	uint32_t glowColor = colorAlpha(0x0078FF, 0.4f); // PS5 Blue Accent

	for (int i = 0; i < numBars; i++) {
		float angle = (i / (float)numBars) * 2.0f * 3.14159f;
		float barVal = 10.0f + sinf(angle * 4.0f + (float)time_now_d() * 3.0f) * 15.0f * (0.2f + visualizerAmplitude_);

		float x0 = centerX + cosf(angle) * baseRadius;
		float y0 = centerY + sinf(angle) * baseRadius;
		float x1 = centerX + cosf(angle) * (baseRadius + barVal);
		float y1 = centerY + sinf(angle) * (baseRadius + barVal);

		dc.Draw()->Line(ImageID("I_SOLIDWHITE"), x0, y0, x1, y1, 4.0f, glowColor);
	}

	// Draw center rotating disc
	dc.Flush();
	dc.Begin();
	float rotationAngle = isPlaying_ ? (float)time_now_d() * 0.5f : 0.0f;
	dc.Draw()->DrawImageRotated(ImageID("I_CIRCLE"), centerX, centerY, 2.5f, rotationAngle, colorAlpha(0x0078FF, 0.2f));
	dc.Draw()->DrawImageRotated(ImageID("I_CIRCLE"), centerX, centerY, 1.0f, -rotationAngle * 2.0f, colorAlpha(0xFFFFFF, 0.15f));

	dc.Flush();
}

// Event Handlers
void MusicPlayerScreen::OnTrackClick(UI::EventParams &e) {
	// Replaced by safe lambdas in CreateViews
}

void MusicPlayerScreen::OnPlayPauseClick(UI::EventParams &e) {
	if (isPlaying_) {
		isPlaying_ = false;
	} else {
		if (currentTrackIdx_ < 0 && !tracks_.empty()) {
			StartPlayback(0);
		} else {
			isPlaying_ = true;
			if (playThread_.joinable()) {
				playThread_.join();
			}
			playThread_ = std::thread(&MusicPlayerScreen::PlayThreadFunc, this);
		}
	}
	RecreateViews();
}

void MusicPlayerScreen::OnStopClick(UI::EventParams &e) {
	StopPlayback();
	RecreateViews();
}

void MusicPlayerScreen::OnNextClick(UI::EventParams &e) {
	if (tracks_.empty()) return;
	int nextIdx = (currentTrackIdx_ + 1) % (int)tracks_.size();
	StartPlayback(nextIdx);
	RecreateViews();
}

void MusicPlayerScreen::OnPrevClick(UI::EventParams &e) {
	if (tracks_.empty()) return;
	int prevIdx = currentTrackIdx_ - 1;
	if (prevIdx < 0) prevIdx = (int)tracks_.size() - 1;
	StartPlayback(prevIdx);
	RecreateViews();
}
