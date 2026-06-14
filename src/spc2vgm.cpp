#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#define private public
#include "SNES_SPC.h"
#undef private
#include "player/playera.hpp"
#include "player/vgmplayer.hpp"
#include "utils/MemoryLoader.h"

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

constexpr size_t SPC_RAM_OFFSET = 0x100, SPC_RAM_SIZE = 0x10000;
constexpr size_t SPC_DSP_OFFSET = 0x10100, SPC_DSP_SIZE = 0x80;
constexpr uint32_t OPL4_CLOCK = 33868800, OPL4_RAM_SIZE = 0x40000;
constexpr uint32_t OPL4_RAM_ADDRESS = 0x200000, OPL4_ROM_SIZE = 0x200000;
constexpr int OPL4_RAM_WAVE_BASE = 384, OPL4_HEADER_SIZE = 12;
constexpr int OPL4_SAMPLE_BASE = 384 * OPL4_HEADER_SIZE;
constexpr int VGM_HEADER_SIZE = 0x100, SPC_CLOCK = 1024000, VGM_RATE = 44100;

struct Write { int64_t clock; uint8_t reg, value; };
struct Sample {
	int index, start, loop, loop_sample;
	bool looped, used = true;
	Bytes brr;
	std::vector<int16_t> pcm, continuation;
	std::vector<int> srcn_aliases;
};
struct Timing { double duration; std::optional<double> loop_start; };
struct RamImage {
	Bytes bytes;
	std::vector<std::pair<size_t, size_t>> ranges;
	size_t upload_size = 0;
};
struct AudioStats {
	long double square_sum = 0;
	uint64_t samples = 0;
	uint64_t clipped = 0;
	int peak = 0;

	double rms() const { return samples ? std::sqrt(double(square_sum / samples)) : 0; }
};
struct Metadata {
	std::string title, game, system = "Super Nintendo Entertainment System", artist, release_date, creator = "spc2vgm";
	std::string notes = "Converted from SNES SPC music to YMF278B/OPL4 VGM using spc2vgm.";
};
struct Options {
	fs::path input, output, manifest, batch, batch_output, manifest_output;
	std::string creator = "spc2vgm";
	double playback = -1, fallback = 120, header_gain = 0, hardware_gain = 6;
	int minimum_tl = 8, max_samples = 128, solo_voice = -1, jobs = std::max(1u, std::thread::hardware_concurrency());
	bool auto_playback = false, no_loop_detect = false, header_gain_set = false, debug = false, wav = false, prune_samples = false;
};

static thread_local std::vector<Write>* trace_target;
static thread_local int64_t trace_clock_base;
static std::mutex output_mutex;
void spc_trace_dsp_write(int time, int reg, int value) {
	if (trace_target && !(reg & 0x80))
		trace_target->push_back({trace_clock_base + time, uint8_t(reg), uint8_t(value)});
}

static Bytes read_file(const fs::path& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) throw std::runtime_error("unable to open " + path.string());
	return Bytes(std::istreambuf_iterator<char>(in), {});
}
static void write_file(const fs::path& path, const Bytes& data) {
	fs::create_directories(path.parent_path());
	std::ofstream out(path, std::ios::binary);
	if (!out) throw std::runtime_error("unable to write " + path.string());
	out.write(reinterpret_cast<const char*>(data.data()), data.size());
}
static uint16_t le16(const uint8_t* p) { return p[0] | uint16_t(p[1]) << 8; }
static uint32_t le32(const uint8_t* p) { return le16(p) | uint32_t(le16(p + 2)) << 16; }
static void put16(Bytes& b, size_t p, uint16_t v) { b[p] = v; b[p + 1] = v >> 8; }
static void put32(Bytes& b, size_t p, uint32_t v) { put16(b, p, v); put16(b, p + 2, v >> 16); }
static void append32(Bytes& b, uint32_t v) {
	for (int i = 0; i < 4; ++i) b.push_back(v >> (i * 8));
}
static std::string fixed_text(const Bytes& data, size_t offset, size_t length) {
	if (offset >= data.size()) return {};
	const size_t end = std::min(data.size(), offset + length);
	size_t used = offset;
	while (used < end && data[used]) ++used;
	while (used > offset && data[used - 1] == ' ') --used;
	return std::string(data.begin() + offset, data.begin() + used);
}
static Metadata metadata(const Bytes& spc, const Options& options) {
	Metadata out;
	out.title = fixed_text(spc, 0x2e, 32);
	out.game = fixed_text(spc, 0x4e, 32);
	out.artist = fixed_text(spc, 0xb1, 32);
	out.creator = options.creator;
	return out;
}
static void append_utf16(Bytes& out, const std::string& text) {
	for (size_t i = 0; i < text.size();) {
		uint32_t code = uint8_t(text[i++]);
		auto continuation = [&](size_t position) { return position < text.size() && (uint8_t(text[position]) & 0xc0) == 0x80; };
		if ((code & 0xe0) == 0xc0 && continuation(i))
			code = ((code & 0x1f) << 6) | (uint8_t(text[i++]) & 0x3f);
		else if ((code & 0xf0) == 0xe0 && continuation(i) && continuation(i + 1)) {
			const uint8_t second = text[i++], third = text[i++];
			code = ((code & 0x0f) << 12) | ((second & 0x3f) << 6) | (third & 0x3f);
		} else if ((code & 0xf8) == 0xf0 && continuation(i) && continuation(i + 1) && continuation(i + 2)) {
			const uint8_t second = text[i++], third = text[i++], fourth = text[i++];
			code = ((code & 7) << 18) | ((second & 0x3f) << 12) | ((third & 0x3f) << 6) | (fourth & 0x3f);
		}
		if (code <= 0xffff) {
			out.push_back(code); out.push_back(code >> 8);
		}
		else {
			code -= 0x10000;
			const uint16_t high = 0xd800 | (code >> 10), low = 0xdc00 | (code & 0x3ff);
			out.push_back(high); out.push_back(high >> 8); out.push_back(low); out.push_back(low >> 8);
		}
	}
	out.insert(out.end(), {0, 0});
}
static Bytes gd3(const Metadata& metadata) {
	Bytes fields;
	for (const std::string* field : std::array<const std::string*, 11>{
		&metadata.title, nullptr, &metadata.game, nullptr, &metadata.system, nullptr,
		&metadata.artist, nullptr, &metadata.release_date, &metadata.creator, &metadata.notes
	}) append_utf16(fields, field ? *field : std::string{});
	Bytes out{'G', 'd', '3', ' '}; append32(out, 0x100); append32(out, fields.size());
	out.insert(out.end(), fields.begin(), fields.end());
	return out;
}
static int clamp16(int v) { return std::clamp(v, -32768, 32767); }
static int signed8(int v) { return (v & 0x80) ? v - 256 : v; }

static void decode_block(const uint8_t* block, int& prev1, int& prev2, std::vector<int16_t>& out) {
	int shift = block[0] >> 4, filter = (block[0] >> 2) & 3;
	for (int i = 1; i < 9; ++i) for (int half = 0; half < 2; ++half) {
		int raw = half ? block[i] & 15 : block[i] >> 4;
		int s = raw & 8 ? raw - 16 : raw;
		s = shift <= 12 ? (s << shift) >> 1 : (s < 0 ? -0x800 : 0);
		if (filter == 1) s += (prev1 >> 1) + ((-prev1) >> 5);
		else if (filter == 2) {
			int h = prev2 >> 1; s += prev1 - h + (h >> 4) + ((prev1 * -3) >> 6);
		} else if (filter == 3) {
			int h = prev2 >> 1; s += prev1 - h + ((prev1 * -13) >> 7) + ((h * 3) >> 4);
		}
		s = clamp16(s);
		s = int16_t(uint16_t(s * 2));
		out.push_back(int16_t(s)); prev2 = prev1; prev1 = s;
	}
}

static std::vector<Sample> extract_samples(const uint8_t* ram, const uint8_t* dsp, int maximum) {
	std::vector<Sample> out;
	std::set<std::pair<int, int>> seen;
	int directory = dsp[0x5d] << 8;
	for (int index = 0; index < 128 && int(out.size()) < maximum; ++index) {
		int entry = directory + index * 4;
		if (entry + 4 > int(SPC_RAM_SIZE)) break;
		int start = le16(ram + entry), loop = le16(ram + entry + 2);
		if (!start || start + 9 > int(SPC_RAM_SIZE)) continue;
		if (seen.count({start, loop})) {
			auto existing = std::find_if(out.begin(), out.end(), [=](const Sample& s) {
				return s.start == start && s.loop == loop;
			});
			if (existing != out.end()) existing->srcn_aliases.push_back(index);
			continue;
		}
		Sample s{index, start, loop, 0, false, true, {}, {}, {}, {index}};
		int pos = start, p1 = 0, p2 = 0;
		while (pos + 9 <= int(SPC_RAM_SIZE)) {
			if (pos == loop) s.loop_sample = s.pcm.size();
			s.brr.insert(s.brr.end(), ram + pos, ram + pos + 9);
			decode_block(ram + pos, p1, p2, s.pcm);
			if (ram[pos] & 1) break;
			pos += 9;
		}
		if (s.brr.empty() || s.pcm.empty() || !(s.brr[s.brr.size() - 9] & 1)) continue;
		s.looped = s.brr[s.brr.size() - 9] & 2;
		if (s.looped && s.loop_sample < int(s.pcm.size())) {
			p1 = s.pcm.back(); p2 = s.pcm.size() > 1 ? s.pcm[s.pcm.size() - 2] : 0;
			for (size_t p = (s.loop_sample / 16) * 9; p + 9 <= s.brr.size(); p += 9)
				decode_block(s.brr.data() + p, p1, p2, s.continuation);
		}
		seen.insert({start, loop}); out.push_back(std::move(s));
	}
	return out;
}

static bool mark_unused_samples(std::vector<Sample>& samples, const uint8_t* dsp, const std::vector<Write>& events) {
	std::array<uint8_t, 128> registers{};
	std::copy(dsp, dsp + registers.size(), registers.begin());
	std::set<int> used;
	auto remember_key_ons = [&](int mask) {
		for (int voice = 0; voice < 8; ++voice)
			if (mask & (1 << voice)) used.insert(registers[voice * 16 + 4]);
	};
	remember_key_ons(registers[0x4c] & ~registers[0x5c]);
	for (const auto& event : events) {
		if (event.reg >= 0x80) continue;
		registers[event.reg] = event.value;
		if (event.reg == 0x4c) remember_key_ons(event.value);
	}
	for (auto& sample : samples) {
		sample.used = std::any_of(sample.srcn_aliases.begin(), sample.srcn_aliases.end(),
			[&](int srcn) { return used.count(srcn); });
	}
	return std::any_of(events.begin(), events.end(), [](const Write& event) {
		return event.reg == 0x3d && event.value;
	}) || dsp[0x3d];
}

static void make_header(Bytes& ram, int wave, uint32_t start, int count, int loop) {
	if (count < 1 || count > 0xffff) throw std::runtime_error("OPL4 wave is too long");
	uint8_t* h = ram.data() + wave * OPL4_HEADER_SIZE;
	uint16_t end = uint16_t(0x10000 - count);
	h[0] = 0x80 | ((start >> 16) & 0x3f); h[1] = start >> 8; h[2] = start;
	h[3] = loop >> 8; h[4] = loop; h[5] = end >> 8; h[6] = end;
	h[7] = 0; h[8] = 0xf0; h[9] = 0; h[10] = 0x0f; h[11] = 0;
}

static RamImage build_ram(const std::vector<Sample>& samples, bool sparse, bool noise_used = true) {
	Bytes ram(OPL4_RAM_SIZE);
	std::vector<bool> included(OPL4_RAM_SIZE);
	auto add_range = [&](size_t start, size_t size) {
		if (sparse) std::fill(included.begin() + start, included.begin() + start + size, true);
	};
	size_t normal = 0;
	for (auto& s : samples) normal += (s.pcm.size() + (!s.looped)) * 2;
	int64_t budget = OPL4_RAM_SIZE - OPL4_SAMPLE_BASE - normal - 8192 * 2;
	std::vector<std::tuple<int, int, int>> choices;
	for (int i = 0; i < int(samples.size()); ++i) {
		auto& s = samples[i]; if (!s.looped || s.continuation.empty()) continue;
		int improvement = std::abs(int(s.pcm.back()) - s.pcm[s.loop_sample])
			- std::abs(int(s.continuation.back()) - s.continuation.front());
		if (improvement > 0) choices.emplace_back(improvement, i, s.continuation.size() * 2);
	}
	std::sort(choices.rbegin(), choices.rend());
	std::set<int> stabilized;
	for (auto [improvement, wave, bytes] : choices)
		if (bytes <= budget) { stabilized.insert(wave); budget -= bytes; }
	size_t pos = OPL4_SAMPLE_BASE;
	for (int wave = 0; wave < int(samples.size()); ++wave) {
		auto& s = samples[wave]; std::vector<int16_t> pcm = s.pcm; int loop = s.loop_sample;
		if (stabilized.count(wave)) { loop = pcm.size(); pcm.insert(pcm.end(), s.continuation.begin(), s.continuation.end()); }
		else if (!s.looped) { loop = pcm.size(); pcm.push_back(0); }
		if (pcm.size() > 0xffff) { pcm = s.pcm; loop = s.looped ? s.loop_sample : pcm.size(); if (!s.looped) pcm.push_back(0); }
		if (pos + pcm.size() * 2 > ram.size()) throw std::runtime_error("decoded samples exceed OPL4 RAM");
		make_header(ram, wave, OPL4_RAM_ADDRESS + pos, pcm.size(), loop);
		if (s.used) add_range(wave * OPL4_HEADER_SIZE, OPL4_HEADER_SIZE);
		const size_t sample_start = pos;
		for (int16_t v : pcm) { ram[pos++] = uint16_t(v) >> 8; ram[pos++] = v; }
		if (s.used) add_range(sample_start, pos - sample_start);
	}
	int noise_wave = samples.size(), lfsr = 1; make_header(ram, noise_wave, OPL4_RAM_ADDRESS + pos, 8192, 0);
	if (noise_used) add_range(noise_wave * OPL4_HEADER_SIZE, OPL4_HEADER_SIZE);
	const size_t noise_start = pos;
	for (int i = 0; i < 8192; ++i) {
		int feedback = (lfsr ^ (lfsr >> 1)) & 1; lfsr = (lfsr >> 1) | (feedback << 14);
		int16_t v = lfsr & 1 ? 12000 : -12000; ram[pos++] = uint16_t(v) >> 8; ram[pos++] = v;
	}
	if (noise_used) add_range(noise_start, pos - noise_start);
	ram.resize(pos);
	std::vector<std::pair<size_t, size_t>> ranges;
	if (!sparse) ranges.emplace_back(0, ram.size());
	else for (size_t start = 0; start < ram.size();) {
		while (start < ram.size() && !included[start]) ++start;
		if (start == ram.size()) break;
		size_t end = start;
		while (end < ram.size() && included[end]) ++end;
		while (end < ram.size()) {
			size_t next = end;
			while (next < ram.size() && !included[next]) ++next;
			if (next - end > 15) break;
			end = next;
			while (end < ram.size() && included[end]) ++end;
		}
		ranges.emplace_back(start, end);
		start = end;
	}
	size_t upload_size = 0;
	for (auto [start, end] : ranges) upload_size += end - start;
	return {std::move(ram), std::move(ranges), upload_size};
}

static std::vector<Write> trace_spc(const Bytes& data, double seconds, AudioStats* audio_stats = nullptr) {
	std::vector<Write> writes; trace_target = &writes; trace_clock_base = 0;
	SNES_SPC spc; if (auto e = spc.init()) throw std::runtime_error(e);
	if (auto e = spc.load_spc(data.data(), data.size())) throw std::runtime_error(e);
	std::vector<SNES_SPC::sample_t> audio(2048);
	std::array<int,8> env;
	for(int v=0;v<8;++v)env[v]=spc.dsp.read(v*16+SPC_DSP::v_envx);
	int pairs_left = int(seconds * SNES_SPC::sample_rate + .5);
	// Track which voices received a KON write within the current 32-pair block so we
	// can force an ENVX event at the block boundary even when the final ENVX value
	// equals the previous one.  This handles fast-attack voices (attack_rate==15)
	// whose envelope drops to 0 on KON and returns to full within just a couple of
	// samples – making the block-end ENVX identical to the pre-KON value and thus
	// producing no change-event.  Without the forced event, playback() would never
	// learn that the envelope reset and would keep env[v]==0 after the KON.
	uint8_t kon_dirty = 0; // bitmask of voices that received KON this block
	while (pairs_left > 0) {
		kon_dirty = 0;
		// Bookmark write-vector size before this block so we only scan new entries.
		const size_t block_write_start = writes.size();
		int pairs = std::min(pairs_left, 32);
		if (auto e = spc.play(pairs * 2, audio.data())) throw std::runtime_error(e);
		if (audio_stats) for (int i = 0; i < pairs * 2; ++i) {
			const int value = audio[i];
			audio_stats->square_sum += static_cast<long double>(value) * value;
			audio_stats->peak = std::max(audio_stats->peak, std::abs(value));
			if (std::abs(value) >= 32767) ++audio_stats->clipped;
			++audio_stats->samples;
		}
		trace_clock_base += int64_t(pairs) * (SPC_CLOCK / SNES_SPC::sample_rate); pairs_left -= pairs;
		// Collect KON writes appended by spc_trace_dsp_write during this block.
		for (size_t i = block_write_start; i < writes.size(); ++i)
			if (writes[i].reg == 0x4c)
				kon_dirty |= writes[i].value;
		for(int v=0;v<8;++v){
			int value=spc.dsp.read(v*16+SPC_DSP::v_envx);
			// Force-emit ENVX for voices that received KON this block, even when the
			// sampled value hasn't changed (fast-attack case).
			if(value!=env[v] || (kon_dirty&(1<<v))){
				env[v]=value;
				writes.push_back({trace_clock_base,uint8_t(0x80+v),uint8_t(value)});
			}
		}
	}
	trace_target = nullptr;
	const int64_t final_clock = std::llround(seconds * SPC_CLOCK);
	writes.erase(std::remove_if(writes.begin(), writes.end(), [=](const Write& w) {
		return w.clock < 0 || w.clock > final_clock;
	}), writes.end());
	return writes;
}

static AudioStats render_spc_stats(const Bytes& data, double seconds, int solo)
{
	SNES_SPC spc;
	if (const char* error = spc.init()) throw std::runtime_error(error);
	if (const char* error = spc.load_spc(data.data(), data.size())) throw std::runtime_error(error);
	if (solo >= 0) spc.mute_voices(0xFF & ~(1 << solo));

	AudioStats stats;
	std::vector<SNES_SPC::sample_t> audio(2048);
	int pairs_left = int(seconds * SNES_SPC::sample_rate + .5);
	while (pairs_left > 0) {
		const int pairs = std::min(pairs_left, 1024);
		if (const char* error = spc.play(pairs * 2, audio.data())) throw std::runtime_error(error);
		for (int i = 0; i < pairs * 2; ++i) {
			const int value = audio[i];
			stats.square_sum += static_cast<long double>(value) * value;
			stats.peak = std::max(stats.peak, std::abs(value));
			if (std::abs(value) >= 32767) ++stats.clipped;
			++stats.samples;
		}
		pairs_left -= pairs;
	}
	return stats;
}

static void write_wav_header(std::ofstream& out, uint32_t sample_rate, uint32_t frames)
{
	const uint32_t audio_bytes = frames * 4;
	out.write("RIFF", 4);
	auto write16 = [&](uint16_t value) { out.put(value); out.put(value >> 8); };
	auto write32 = [&](uint32_t value) { write16(value); write16(value >> 16); };
	write32(36 + audio_bytes);
	out.write("WAVEfmt ", 8);
	write32(16); write16(1); write16(2);
	write32(sample_rate); write32(sample_rate * 4); write16(4); write16(16);
	out.write("data", 4); write32(audio_bytes);
}

static void render_spc_wav(const Bytes& data, double seconds, int solo, const fs::path& path)
{
	SNES_SPC spc;
	if (const char* error = spc.init()) throw std::runtime_error(error);
	if (const char* error = spc.load_spc(data.data(), data.size())) throw std::runtime_error(error);
	spc.mute_voices(0xFF & ~(1 << solo));

	const int frames = int(seconds * SNES_SPC::sample_rate + .5);
	fs::create_directories(path.parent_path());
	std::ofstream out(path, std::ios::binary);
	if (!out) throw std::runtime_error("unable to write " + path.string());
	write_wav_header(out, SNES_SPC::sample_rate, frames);
	std::vector<SNES_SPC::sample_t> audio(2048);
	int remaining = frames;
	while (remaining > 0) {
		const int count = std::min(remaining, 1024);
		if (const char* error = spc.play(count * 2, audio.data())) throw std::runtime_error(error);
		out.write(reinterpret_cast<const char*>(audio.data()), count * 2 * sizeof(audio[0]));
		remaining -= count;
	}
}

static Timing timing(const Bytes& d, double fallback) {
	for (size_t off = SPC_DSP_OFFSET + SPC_DSP_SIZE; off + 8 <= d.size(); ++off) if (!std::memcmp(d.data() + off, "xid6", 4)) {
		size_t end = std::min(d.size(), off + 8 + le32(d.data() + off + 4)); std::array<uint32_t, 256> val{};
		for (size_t p = off + 8; p + 4 <= end;) {
			int id = d[p], type = d[p + 1], len = le16(d.data() + p + 2); p += 4;
			if (!type) val[id] = len;
			else { if (type == 4 && p + 4 <= end) val[id] = le32(d.data() + p); p += (len + 3) & ~3; }
		}
		double intro = val[0x30] / 64000.0, loop = val[0x31] / 64000.0;
		double duration = intro + loop * std::max(1u, val[0x35]) + val[0x32] / 64000.0 + val[0x33] / 64000.0;
		if (duration > 0) return {duration, loop > 0 ? std::optional<double>(intro) : std::nullopt};
	}
	return {fallback, std::nullopt};
}

using Signature = std::array<uint32_t, 8>;
static std::optional<std::pair<double, double>> detect_loop(const std::vector<Write>& events, const uint8_t* dsp) {
	std::array<uint8_t, 128> regs{}; std::copy(dsp, dsp + 128, regs.begin());
	std::vector<std::pair<int64_t, Signature>> notes;
	for (auto e : events) {
		if(e.reg>=0x80)continue;
		regs[e.reg] = e.value; if (e.reg != 0x4c || !e.value) continue;
		Signature s{}; for (int v = 0; v < 8; ++v) if (e.value & (1 << v))
			s[v] = 0x80000000u | regs[v * 16 + 4] << 16 | regs[v * 16 + 2] | ((regs[v * 16 + 3] & 0x3f) << 8);
		notes.emplace_back(e.clock, s);
	}
	const int window = 48; int best_a = -1, best_b = -1, best_n = 0;
	for (int b = window; b + window < int(notes.size()); ++b) {
		if (notes[b].first < int64_t(SPC_CLOCK) * 8) continue;
		for (int a = 0; a + window <= b - window; ++a) {
			if (notes[b].first - notes[a].first < int64_t(SPC_CLOCK) * 5) continue;
			bool initial_match = true;
			for (int n = 0; n < window; ++n) if (notes[a + n].second != notes[b + n].second) {
				initial_match = false; break;
			}
			if (!initial_match) continue;
			int n = window;
			while (b + n < int(notes.size()) && notes[a + n].second == notes[b + n].second) {
				if (std::llabs((notes[a+n].first-notes[a+n-1].first)-(notes[b+n].first-notes[b+n-1].first)) > 64) break;
				++n; if (a + n >= b) break;
			}
			if (n >= window && n > best_n) { best_a = a; best_b = b; best_n = n; }
		}
	}
	if (best_a < 0) return std::nullopt;
	return std::pair<double,double>{notes[best_a].first / double(SPC_CLOCK), notes[best_b].first / double(SPC_CLOCK)};
}

static void reg(Bytes& o, int port, int r, int v) { o.insert(o.end(), {0xd0, uint8_t(port), uint8_t(r), uint8_t(v)}); }
static void wait(Bytes& o, int64_t n) { while (n > 0) { int step = std::min<int64_t>(n, 65535); o.push_back(0x61); o.push_back(step); o.push_back(step >> 8); n -= step; } }
static std::pair<int,int> pitch(int p) {
	p = std::max(1, p & 0x3fff); double target = (p / 4096.0) * (32000.0 / 44100.0) * 65536.0, error = 1e100; int bo = 0, bf = 0;
	for (int o = -8; o < 8; ++o) { double scale = std::ldexp(1.0, 5 + o), f = target / scale - 1024; int fn = std::lround(f);
		if (fn >= 0 && fn <= 1023 && std::abs((fn + 1024) * scale - target) < error) { error = std::abs((fn + 1024) * scale - target); bo = o; bf = fn; } }
	return {bf, bo & 15};
}
static std::pair<int,int> level_pan(const std::array<uint8_t,128>& r, int v, double gain, int minimum, int envx=-1) {
	int b = v * 16; double l = std::abs(signed8(r[b]) * signed8(r[0x0c])) / 127.0, rr = std::abs(signed8(r[b+1]) * signed8(r[0x1c])) / 127.0;
	double env = envx>=0 ? envx/127.0 : ((!(r[b+5] & 0x80) && r[b+7] < 0x80) ? r[b+7] / 127.0 : 1.0), mx = std::max(l, rr), amp = mx / 127.0 * env;
	if (amp <= 0) return {127, 8}; int tl = std::clamp(int(std::lround((-20 * std::log10(std::min(1.0, amp)) - gain) / .375)), minimum, 126);
	static const double p[16][2]={{1,1},{.70795,1},{.50119,1},{.35481,1},{.25119,1},{.17783,1},{.12589,1},{0,1},{0,0},{1,0},{1,.12589},{1,.17783},{1,.25119},{1,.35481},{1,.50119},{1,.70795}};
	double tlr=l/mx,trr=rr/mx,best=1e9; int pan=0; for(int i=0;i<16;++i)if(i!=8){double e=(p[i][0]-tlr)*(p[i][0]-tlr)+(p[i][1]-trr)*(p[i][1]-trr);if(e<best){best=e;pan=i;}}
	return {tl,pan};
}
static void wave_pitch(Bytes& o,int v,int wave,int p){auto [fn,oct]=pitch(p);reg(o,2,0x20+v,((wave>>8)&1)|((fn&127)<<1));reg(o,2,0x38+v,(oct<<4)|((fn>>7)&7));}
static void level(Bytes& o,int v,const std::array<uint8_t,128>& r,double gain,int minimum,bool on,int envx=-1){auto [tl,pan]=level_pan(r,v,gain,minimum,envx);reg(o,2,0x50+v,(tl<<1)|1);reg(o,2,0x68+v,pan|(on?0x80:0));}
static void key_on(Bytes&o,int slot,int source,int wave,const std::array<uint8_t,128>&r,double gain,int minimum,int envx){
	int b=source*16,p=r[b+2]|((r[b+3]&0x3f)<<8);auto[fn,oct]=pitch(p);reg(o,2,0x20+slot,((wave>>8)&1)|((fn&127)<<1));reg(o,2,0x38+slot,(oct<<4)|((fn>>7)&7)|((r[0x4d]&(1<<source))?8:0));reg(o,2,8+slot,wave);auto q=level_pan(r,source,gain,minimum,envx);reg(o,2,0x50+slot,(q.first<<1)|1);reg(o,2,0x68+slot,q.second);reg(o,2,0x80+slot,(r[0x2d]&(1<<source))?7:0);reg(o,2,0x98+slot,0xf0);reg(o,2,0xb0+slot,0);reg(o,2,0xc8+slot,0xf0);reg(o,2,0xe0+slot,0);reg(o,2,0x68+slot,q.second|0x80);
}

struct Tail { Bytes bytes; int total; std::optional<size_t> loop_offset; int loop_sample; };
static void append_optimized_wait(Bytes& out, int64_t samples) {
	while (samples > 0) {
		if (samples == 735) { out.push_back(0x62); return; }
		if (samples == 882) { out.push_back(0x63); return; }
		if (samples <= 32) {
			int step = std::min<int64_t>(samples, 16);
			out.push_back(uint8_t(0x70 + step - 1));
			samples -= step;
			continue;
		}
		int step = std::min<int64_t>(samples, 65535);
		out.insert(out.end(), {0x61, uint8_t(step), uint8_t(step >> 8)});
		samples -= step;
	}
}
static Tail optimize_tail(Tail in) {
	Tail out{{}, in.total, std::nullopt, in.loop_sample};
	std::array<int, 0x300> registers; registers.fill(-1);
	size_t pos = 0;
	int64_t pending_wait = 0;
	auto flush_wait = [&] { append_optimized_wait(out.bytes, pending_wait); pending_wait = 0; };
	while (pos < in.bytes.size()) {
		if (in.loop_offset && pos == *in.loop_offset) {
			flush_wait();
			out.loop_offset = out.bytes.size();
			registers.fill(-1);
		}
		const uint8_t command = in.bytes[pos];
		if (command == 0x61 && pos + 2 < in.bytes.size()) {
			pending_wait += in.bytes[pos + 1] | (in.bytes[pos + 2] << 8);
			pos += 3;
			continue;
		}
		if (command == 0x62 || command == 0x63 || (command >= 0x70 && command <= 0x7f)) {
			pending_wait += command == 0x62 ? 735 : command == 0x63 ? 882 : (command & 15) + 1;
			++pos;
			continue;
		}
		flush_wait();
		if (command == 0xd0 && pos + 3 < in.bytes.size()) {
			const int index = in.bytes[pos + 1] * 0x100 + in.bytes[pos + 2];
			const int value = in.bytes[pos + 3];
			if (registers[index] != value) {
				out.bytes.insert(out.bytes.end(), in.bytes.begin() + pos, in.bytes.begin() + pos + 4);
				registers[index] = value;
			}
			pos += 4;
			continue;
		}
		out.bytes.push_back(command);
		++pos;
	}
	flush_wait();
	if (in.loop_offset && *in.loop_offset == in.bytes.size()) out.loop_offset = out.bytes.size();
	return out;
}
static Tail playback(const uint8_t*dsp,const std::vector<Sample>& samples,const std::vector<Write>&events,double seconds,double gain,int minimum,int solo,std::optional<double> loop_start,bool include_direct=true,bool include_echo=true){
	std::array<int,128> map;map.fill(-1);for(int i=0;i<int(samples.size());++i)for(int srcn:samples[i].srcn_aliases)map[srcn]=OPL4_RAM_WAVE_BASE+i;
	std::array<uint8_t,128> r{};std::copy(dsp,dsp+128,r.begin());std::array<bool,8> active{};std::array<int,8> aw{},env{};Bytes o;reg(o,1,5,2);reg(o,2,2,0x10);reg(o,2,0xf9,0);
	for(int v=0;v<8;++v)env[v]=dsp[v*16+8];
	int noise=OPL4_RAM_WAVE_BASE+samples.size();auto wave_for=[&](int v){return(r[0x3d]&(1<<v))?noise:map[r[v*16+4]];};
	for(int v=0;v<8;++v)if(include_direct&&(solo<0||solo==v)&&(dsp[0x4c]&(1<<v))&&!(dsp[0x5c]&(1<<v))&&wave_for(v)>=0){aw[v]=wave_for(v);key_on(o,v,v,aw[v],r,gain,minimum,env[v]);active[v]=true;}
	struct EchoHit{int64_t time;int slot,source,wave,note;double gain;bool on;std::array<uint8_t,128> regs;};
	std::vector<EchoHit> echo_hits;std::array<uint8_t,128> er=r;
	int echo_note=0;
	for(size_t ei=0;ei<events.size();++ei){auto e=events[ei];
		if(e.reg>=0x80)continue;er[e.reg]=e.value;if(e.reg!=0x4c)continue;
		for(int v=0;v<8;++v)if(include_echo&&(e.value&(1<<v))&&(solo<0||solo==v)&&(er[0x4d]&(1<<v))){
			int wave=(er[0x3d]&(1<<v))?noise:map[er[v*16+4]];
			int sample=wave-OPL4_RAM_WAVE_BASE;if(sample<0||sample>=int(samples.size()))continue;
			int delay=std::max(1,er[0x7d]&15)*16*VGM_RATE/1000;double master=std::max(std::abs(signed8(er[0x0c])),std::abs(signed8(er[0x1c]))),echo=std::max(std::abs(signed8(er[0x2c])),std::abs(signed8(er[0x3c])));
			if(master<=0||echo<=0)continue;double echo_gain=gain+20*std::log10(echo/master),feedback=std::abs(signed8(er[0x0d]))/128.0;
			int64_t base=std::llround(e.clock*double(VGM_RATE)/SPC_CLOCK);double amp=1.0;
			int64_t end=0;if(samples[sample].looped)for(size_t j=ei+1;j<events.size();++j)if((events[j].reg==0x5c||events[j].reg==0x4c)&&(events[j].value&(1<<v))){end=std::llround(events[j].clock*double(VGM_RATE)/SPC_CLOCK);break;}
			if(samples[sample].looped&&!end)continue;
			if(samples[sample].looped)echo_gain-=6.0206;
			for(int k=1;k<=(samples[sample].looped?1:16);++k){int slot=(k&1)?8+v:16+v,note=echo_note++;echo_hits.push_back({base+(int64_t)k*delay,slot,v,wave,note,echo_gain+20*std::log10(amp),true,er});if(end)echo_hits.push_back({end+(int64_t)k*delay,slot,v,wave,note,0,false,er});if(feedback<=0)break;amp*=feedback;if(amp<0.03)break;}
		}
	}
	std::sort(echo_hits.begin(),echo_hits.end(),[](const EchoHit&a,const EchoHit&b){return a.time!=b.time?a.time<b.time:a.on<b.on;});size_t next_echo=0;std::array<int,24> echo_active;echo_active.fill(-1);
	int64_t last=0,ls=loop_start?std::llround(*loop_start*VGM_RATE):0;std::optional<size_t> lo;
	auto wait_to=[&](int64_t target){if(loop_start&&!lo&&last<=ls&&ls<=target){wait(o,ls-last);last=ls;lo=o.size();}if(target>last){wait(o,target-last);last=target;}};
	auto flush_echo=[&](int64_t target){while(next_echo<echo_hits.size()&&echo_hits[next_echo].time<=target){auto&h=echo_hits[next_echo++];wait_to(h.time);if(h.on){key_on(o,h.slot,h.source,h.wave,h.regs,h.gain,minimum,127);echo_active[h.slot]=h.note;}else if(echo_active[h.slot]==h.note){reg(o,2,0x68+h.slot,level_pan(h.regs,h.source,0,minimum,127).second|0x40);echo_active[h.slot]=-1;}}wait_to(target);};
	for(auto e:events){int64_t t=std::llround(e.clock*double(VGM_RATE)/SPC_CLOCK);flush_echo(t);
		if(e.reg>=0x80){int v=e.reg-0x80;env[v]=e.value;if((solo<0||solo==v)&&active[v])level(o,v,r,gain,minimum,true,env[v]);continue;}
		r[e.reg]=e.value;
		if(e.reg==0x4c){for(int v=0;v<8;++v)if(include_direct&&(e.value&(1<<v))&&(solo<0||solo==v)&&wave_for(v)>=0){
			// Keep the sampled pre-KON ENVX because a fast ADSR attack can complete
			// between ENVX polling intervals.
			aw[v]=wave_for(v);key_on(o,v,v,aw[v],r,gain,minimum,env[v]);active[v]=true;}}
		else if(e.reg==0x5c){}
		else if(e.reg==0x0c||e.reg==0x1c){for(int v=0;v<8;++v)if(active[v]&&(solo<0||solo==v))level(o,v,r,gain,minimum,true,env[v]);}
		else if(e.reg==0x2d||e.reg==0x3d||e.reg==0x4d){for(int v=0;v<8;++v)if(active[v]&&(solo<0||solo==v)){if(e.reg==0x2d)reg(o,2,0x80+v,(e.value&(1<<v))?7:0);else if(e.reg==0x4d){int b=v*16;auto[fn,oct]=pitch(r[b+2]|((r[b+3]&63)<<8));reg(o,2,0x38+v,(oct<<4)|((fn>>7)&7)|((e.value&(1<<v))?8:0));}else{int w=wave_for(v);if(w>=0&&w!=aw[v]){int b=v*16;wave_pitch(o,v,w,r[b+2]|((r[b+3]&63)<<8));reg(o,2,8+v,w);aw[v]=w;}}}}
		else{int v=e.reg>>4,s=e.reg&15;if(v<8&&active[v]&&(solo<0||solo==v)){if(s==0||s==1||s==5||s==7)level(o,v,r,gain,minimum,true,env[v]);if(s==2||s==3){int b=v*16;wave_pitch(o,v,aw[v],r[b+2]|((r[b+3]&63)<<8));}}}
	}
	int total=std::max<int64_t>(last,std::llround(seconds*VGM_RATE));flush_echo(total);return optimize_tail({std::move(o),total,lo,int(ls)});
}

static Bytes build_vgm(const RamImage&ram,const Tail&t,double gain,const Metadata* metadata=nullptr){
	Bytes cmd={0x67,0x66,0x84};append32(cmd,8);append32(cmd,OPL4_ROM_SIZE);append32(cmd,0);
	for(auto [start,end]:ram.ranges){cmd.insert(cmd.end(),{0x67,0x66,0x87});append32(cmd,8+end-start);append32(cmd,OPL4_RAM_SIZE);append32(cmd,start);cmd.insert(cmd.end(),ram.bytes.begin()+start,ram.bytes.begin()+end);}size_t tail=cmd.size();cmd.insert(cmd.end(),t.bytes.begin(),t.bytes.end());cmd.push_back(0x66);
	Bytes v(VGM_HEADER_SIZE);std::copy_n("Vgm ",4,v.begin());put32(v,8,0x171);put32(v,0x18,t.total);put32(v,0x24,60);put32(v,0x34,VGM_HEADER_SIZE-0x34);put32(v,0x60,OPL4_CLOCK);v[0x7c]=uint8_t(std::clamp(int(std::lround(gain*32/6)), -64,192));
	if(t.loop_offset){uint32_t absolute=VGM_HEADER_SIZE+tail+*t.loop_offset;put32(v,0x1c,absolute-0x1c);put32(v,0x20,t.total-t.loop_sample);}
	v.insert(v.end(),cmd.begin(),cmd.end());
	if(metadata){put32(v,0x14,v.size()-0x14);Bytes block=gd3(*metadata);v.insert(v.end(),block.begin(),block.end());}
	put32(v,4,v.size()-4);return v;
}
static AudioStats render_vgm_stats(const Bytes& vgm, int frames)
{
	PlayerA player;
	player.RegisterPlayerEngine(new VGMPlayer);
	if (player.SetOutputSettings(VGM_RATE, 2, 16, 2048))
		throw std::runtime_error("libvgm rejected calibration output settings");
	PlayerA::Config config = player.GetConfiguration();
	config.masterVol = 0x10000;
	config.ignoreVolGain = false;
	config.loopCount = 1;
	config.fadeSmpls = 0;
	config.endSilenceSmpls = 0;
	player.SetConfiguration(config);

	DATA_LOADER* loader = MemoryLoader_Init(vgm.data(), vgm.size());
	if (!loader || MemoryLoader_Load(loader) || player.LoadFile(loader) || player.Start()) {
		if (loader) MemoryLoader_Deinit(loader);
		throw std::runtime_error("unable to render generated VGM for volume calibration");
	}

	AudioStats stats;
	std::vector<int16_t> audio(2048 * 2);
	int remaining = frames;
	while (remaining > 0) {
		const int count = std::min(remaining, 2048);
		player.Render(count * 2 * sizeof(int16_t), audio.data());
		for (int i = 0; i < count * 2; ++i) {
			const int value = audio[i];
			stats.square_sum += static_cast<long double>(value) * value;
			stats.peak = std::max(stats.peak, std::abs(value));
			if (std::abs(value) >= 32767) ++stats.clipped;
			++stats.samples;
		}
		remaining -= count;
	}
	player.Stop();
	player.UnloadFile();
	player.UnregisterAllPlayers();
	MemoryLoader_Deinit(loader);
	return stats;
}
static void render_vgm_wav(const Bytes& vgm, int frames, const fs::path& path)
{
	PlayerA player;
	player.RegisterPlayerEngine(new VGMPlayer);
	if (player.SetOutputSettings(VGM_RATE, 2, 16, 2048))
		throw std::runtime_error("libvgm rejected debug output settings");
	PlayerA::Config config = player.GetConfiguration();
	config.masterVol = 0x10000; config.ignoreVolGain = false; config.loopCount = 1;
	config.fadeSmpls = 0; config.endSilenceSmpls = 0;
	player.SetConfiguration(config);
	DATA_LOADER* loader = MemoryLoader_Init(vgm.data(), vgm.size());
	if (!loader || MemoryLoader_Load(loader) || player.LoadFile(loader) || player.Start()) {
		if (loader) MemoryLoader_Deinit(loader);
		throw std::runtime_error("unable to render generated VGM debug channel");
	}

	fs::create_directories(path.parent_path());
	std::ofstream out(path, std::ios::binary);
	if (!out) throw std::runtime_error("unable to write " + path.string());
	write_wav_header(out, VGM_RATE, frames);
	std::vector<int16_t> audio(2048 * 2);
	int remaining = frames;
	while (remaining > 0) {
		const int count = std::min(remaining, 2048);
		player.Render(count * 4, audio.data());
		out.write(reinterpret_cast<const char*>(audio.data()), count * 4);
		remaining -= count;
	}
	player.Stop(); player.UnloadFile(); player.UnregisterAllPlayers(); MemoryLoader_Deinit(loader);
}

static double matched_header_gain(const Bytes& data, const RamImage& ram, const Tail& tail, double seconds, int solo, bool parallel, const AudioStats* traced_original = nullptr)
{
	const Bytes provisional = build_vgm(ram, tail, 0);
	AudioStats original, converted;
	if (traced_original) {
		original = *traced_original;
		converted = render_vgm_stats(provisional, tail.total);
	} else if (parallel) {
		auto original_future = std::async(std::launch::async, [&] { return render_spc_stats(data, seconds, solo); });
		converted = render_vgm_stats(provisional, tail.total);
		original = original_future.get();
	} else {
		original = render_spc_stats(data, seconds, solo);
		converted = render_vgm_stats(provisional, tail.total);
	}
	if (original.rms() <= 0 || converted.rms() <= 0) return 0;
	const double wanted_steps = std::log2(original.rms() / converted.rms()) * 32.0;
	const double safe_steps = converted.peak > 0 ? std::log2(32767.0 / converted.peak) * 32.0 : wanted_steps;
	const int gain_steps = std::clamp(int(std::lround(std::min(wanted_steps, safe_steps))), -64, 192);
	const double gain = gain_steps * 6.0 / 32.0;
	const double gain_factor = std::pow(2.0, gain_steps / 32.0);
	const double matched_rms = converted.rms() * gain_factor;
	const uint64_t matched_clipped = gain_steps == 0 ? converted.clipped : 0;
	std::lock_guard<std::mutex> lock(output_mutex);
	std::cout << "volume match: SPC RMS " << original.rms() << ", VGM RMS " << matched_rms
		<< ", gain " << gain << " dB, error "
		<< 20.0 * std::log10(matched_rms / original.rms()) << " dB, clipped "
		<< matched_clipped << "\n";
	return gain;
}
static void manifest(const fs::path&p,const std::vector<Sample>&s){
	fs::create_directories(p.parent_path());std::ofstream o(p);o<<"wave,srcn,srcn_aliases,start_hex,loop_hex,brr_bytes,pcm_samples,loop_sample,looped\n";
	for(int i=0;i<int(s.size());++i)if(s[i].used){o<<i<<','<<s[i].index<<',';for(size_t a=0;a<s[i].srcn_aliases.size();++a){if(a)o<<'|';o<<s[i].srcn_aliases[a];}o<<",0x"<<std::hex<<std::uppercase<<std::setw(4)<<std::setfill('0')<<s[i].start<<",0x"<<std::setw(4)<<s[i].loop<<std::dec<<','<<s[i].brr.size()<<','<<s[i].pcm.size()<<','<<s[i].loop_sample<<','<<s[i].looped<<'\n';}
}
static void convert(const fs::path&in,const fs::path&out,const fs::path&csv,const Options&opt){
	Bytes d=read_file(in);if(d.size()<SPC_DSP_OFFSET+SPC_DSP_SIZE||std::memcmp(d.data(),"SNES-SPC700 Sound File Data",25))throw std::runtime_error("invalid SPC: "+in.string());
	auto samples=extract_samples(d.data()+SPC_RAM_OFFSET,d.data()+SPC_DSP_OFFSET,opt.max_samples);Timing ti=timing(d,opt.fallback);double seconds=opt.playback>=0?opt.playback:ti.duration;const double traced_seconds=seconds;AudioStats traced_original;auto events=trace_spc(d,seconds,opt.solo_voice<0?&traced_original:nullptr);auto loop=ti.loop_start;
	if(!loop&&!opt.no_loop_detect)if(auto found=detect_loop(events,d.data()+SPC_DSP_OFFSET)){loop=found->first;seconds=found->second;}
	const int64_t playback_end = std::llround(seconds * SPC_CLOCK);
	events.erase(std::remove_if(events.begin(), events.end(), [=](const Write& w) {
		return w.clock > playback_end;
	}), events.end());
	bool noise_used=true;if(opt.prune_samples)noise_used=mark_unused_samples(samples,d.data()+SPC_DSP_OFFSET,events);auto ram=build_ram(samples,opt.prune_samples,noise_used);
	Tail tail=playback(d.data()+SPC_DSP_OFFSET,samples,events,seconds,opt.hardware_gain,opt.minimum_tl,opt.solo_voice,loop);
	double header_gain = opt.header_gain;
	if (!opt.header_gain_set) header_gain = matched_header_gain(d, ram, tail, seconds, opt.solo_voice, opt.jobs > 1, opt.solo_voice < 0 && seconds == traced_seconds ? &traced_original : nullptr);
	Metadata tags=metadata(d,opt);if(tags.title.empty())tags.title=in.stem().string();
	const Bytes vgm=build_vgm(ram,tail,header_gain,&tags);
	write_file(out,vgm);if(!csv.empty())manifest(csv,samples);
	{
		std::lock_guard<std::mutex> lock(output_mutex);
		std::cout<<"wrote "<<out<<" ("<<std::count_if(samples.begin(),samples.end(),[](const Sample&s){return s.used;})<<" samples, "<<ram.upload_size<<" OPL4 RAM bytes, "<<seconds<<" seconds";
		if(loop)std::cout<<", loop "<<*loop<<"s";std::cout<<")\n";
	}
	if(opt.wav){
		const fs::path wav=fs::current_path()/(in.stem().string()+".wav");
		render_vgm_wav(vgm,tail.total,wav);
		std::cout<<"wrote "<<wav<<"\n";
	}
}
static void debug_export(const fs::path& input, const Options& opt)
{
	Bytes data = read_file(input);
	if(data.size()<SPC_DSP_OFFSET+SPC_DSP_SIZE||std::memcmp(data.data(),"SNES-SPC700 Sound File Data",25))
		throw std::runtime_error("invalid SPC: "+input.string());
	auto samples=extract_samples(data.data()+SPC_RAM_OFFSET,data.data()+SPC_DSP_OFFSET,opt.max_samples);
	Timing ti=timing(data,opt.fallback);double seconds=opt.playback>=0?opt.playback:ti.duration;const double traced_seconds=seconds;
	AudioStats traced_original;auto events=trace_spc(data,seconds,&traced_original);auto loop=ti.loop_start;
	if(!loop&&!opt.no_loop_detect)if(auto found=detect_loop(events,data.data()+SPC_DSP_OFFSET)){loop=found->first;seconds=found->second;}
	const int64_t end=std::llround(seconds*SPC_CLOCK);
	events.erase(std::remove_if(events.begin(),events.end(),[=](const Write&w){return w.clock>end;}),events.end());
	bool noise_used=true;if(opt.prune_samples)noise_used=mark_unused_samples(samples,data.data()+SPC_DSP_OFFSET,events);auto ram=build_ram(samples,opt.prune_samples,noise_used);
	Tail complete=playback(data.data()+SPC_DSP_OFFSET,samples,events,seconds,opt.hardware_gain,opt.minimum_tl,-1,loop);
	const double gain=opt.header_gain_set?opt.header_gain:matched_header_gain(data,ram,complete,seconds,-1,opt.jobs>1,seconds==traced_seconds?&traced_original:nullptr);
	const fs::path dir=fs::current_path()/"debug";const std::string stem=input.stem().string();
	for(int voice=0;voice<8;++voice){
		const fs::path original=dir/(stem+"-spc-voice-"+std::to_string(voice)+".wav");
		const fs::path converted=dir/(stem+"-vgm-voice-"+std::to_string(voice)+".wav");
		const fs::path direct=dir/(stem+"-vgm-voice-"+std::to_string(voice)+"-direct.wav");
		const fs::path echo=dir/(stem+"-vgm-voice-"+std::to_string(voice)+"-echo.wav");
		std::cout<<"rendering voice "<<voice<<" original SPC\n";
		render_spc_wav(data,seconds,voice,original);
		Tail solo=playback(data.data()+SPC_DSP_OFFSET,samples,events,seconds,opt.hardware_gain,opt.minimum_tl,voice,loop);
		Tail solo_direct=playback(data.data()+SPC_DSP_OFFSET,samples,events,seconds,opt.hardware_gain,opt.minimum_tl,voice,loop,true,false);
		Tail solo_echo=playback(data.data()+SPC_DSP_OFFSET,samples,events,seconds,opt.hardware_gain,opt.minimum_tl,voice,loop,false,true);
		std::cout<<"rendering voice "<<voice<<" converted VGM\n";
		render_vgm_wav(build_vgm(ram,solo,gain),solo.total,converted);
		render_vgm_wav(build_vgm(ram,solo_direct,gain),solo_direct.total,direct);
		render_vgm_wav(build_vgm(ram,solo_echo,gain),solo_echo.total,echo);
		std::cout<<"wrote "<<original<<"\n"<<"wrote "<<converted<<"\n"<<"wrote "<<direct<<"\n"<<"wrote "<<echo<<"\n";
	}
}
static void usage(const char*n){std::cerr<<"usage: "<<n<<" input.spc [-o output.vgm] [--creator NAME] [--wav] [--manifest file.csv] [--auto-playback] [--prune-samples] [--jobs N]\n       "<<n<<" --debug input.spc [--playback SECONDS] [--prune-samples] [--jobs N]\n       "<<n<<" --batch DIR [--batch-output DIR] [--creator NAME] [--manifest-output DIR] [--prune-samples] [--jobs N]\n"; }
static Options parse(int ac,char**av){
	Options o;for(int i=1;i<ac;++i){std::string a=av[i];auto value=[&](){if(++i>=ac)throw std::runtime_error("missing value after "+a);return std::string(av[i]);};
		if(a=="-o"||a=="--output")o.output=value();else if(a=="--manifest")o.manifest=value();else if(a=="--batch")o.batch=value();else if(a=="--batch-output")o.batch_output=value();else if(a=="--manifest-output")o.manifest_output=value();else if(a=="--creator")o.creator=value();else if(a=="--playback")o.playback=std::stod(value());else if(a=="--fallback-seconds")o.fallback=std::stod(value());else if(a=="--playback-gain-db"){o.header_gain=std::stod(value());o.header_gain_set=true;}else if(a=="--hardware-gain-db")o.hardware_gain=std::stod(value());else if(a=="--minimum-tl")o.minimum_tl=std::stoi(value());else if(a=="--max-samples")o.max_samples=std::stoi(value());else if(a=="--solo-voice")o.solo_voice=std::stoi(value());else if(a=="--jobs")o.jobs=std::stoi(value());else if(a=="--debug")o.debug=true;else if(a=="--wav")o.wav=true;else if(a=="--auto-playback")o.auto_playback=true;else if(a=="--no-loop-detect")o.no_loop_detect=true;else if(a=="--prune-samples")o.prune_samples=true;else if(a=="-h"||a=="--help"){usage(av[0]);std::exit(0);}else if(a[0]=='-')throw std::runtime_error("unknown option "+a);else o.input=a;
	}if(o.jobs<1)throw std::runtime_error("--jobs must be at least 1");return o;
}
int main(int ac,char**av){try{Options o=parse(ac,av);if(!o.batch.empty()){if(o.debug||o.wav)throw std::runtime_error("--debug and --wav only accept a single SPC");fs::path out=o.batch_output.empty()?o.batch/"vgm":o.batch_output;std::vector<fs::path> files;for(auto&e:fs::directory_iterator(o.batch))if(e.path().extension()==".spc")files.push_back(e.path());std::sort(files.begin(),files.end());const int worker_count=std::min<int>(o.jobs,files.size());Options worker_options=o;if(worker_count>1)worker_options.jobs=1;size_t next=0;std::mutex work_mutex;std::exception_ptr failure;auto worker=[&]{for(;;){fs::path p;{std::lock_guard<std::mutex> lock(work_mutex);if(failure||next>=files.size())return;p=files[next++];}try{fs::path csv=o.manifest_output.empty()?fs::path{}:o.manifest_output/(p.stem().string()+".csv");convert(p,out/(p.stem().string()+".vgm"),csv,worker_options);}catch(...){std::lock_guard<std::mutex> lock(work_mutex);if(!failure)failure=std::current_exception();return;}}};std::vector<std::thread> workers;for(int i=0;i<worker_count;++i)workers.emplace_back(worker);for(auto&t:workers)t.join();if(failure)std::rethrow_exception(failure);return 0;}if(o.input.empty()){usage(av[0]);return 2;}if(o.debug){if(o.wav)throw std::runtime_error("--debug and --wav cannot be combined");debug_export(o.input,o);return 0;}if(o.output.empty()){o.output=o.input;o.output.replace_extension(".vgm");}convert(o.input,o.output,o.manifest,o);return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
