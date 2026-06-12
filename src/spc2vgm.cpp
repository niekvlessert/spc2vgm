#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "SNES_SPC.h"

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
	bool looped;
	Bytes brr;
	std::vector<int16_t> pcm, continuation;
	std::vector<int> srcn_aliases;
};
struct Timing { double duration; std::optional<double> loop_start; };
struct Options {
	fs::path input, output, manifest, batch, batch_output, manifest_output;
	double playback = -1, fallback = 120, header_gain = 12, hardware_gain = 6;
	int minimum_tl = 8, max_samples = 128, solo_voice = -1;
	bool auto_playback = false, no_loop_detect = false;
};

static std::vector<Write>* trace_target;
static int64_t trace_clock_base;
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
	std::set<int> seen;
	int directory = dsp[0x5d] << 8;
	for (int index = 0; index < 128 && int(out.size()) < maximum; ++index) {
		int entry = directory + index * 4;
		if (entry + 4 > int(SPC_RAM_SIZE)) break;
		int start = le16(ram + entry), loop = le16(ram + entry + 2);
		if (!start || start + 9 > int(SPC_RAM_SIZE)) continue;
		if (seen.count(start)) {
			auto existing = std::find_if(out.begin(), out.end(), [=](const Sample& s) {
				return s.start == start;
			});
			if (existing != out.end()) existing->srcn_aliases.push_back(index);
			continue;
		}
		Sample s{index, start, loop, 0, false, {}, {}, {}, {index}};
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
		seen.insert(start); out.push_back(std::move(s));
	}
	return out;
}

static void make_header(Bytes& ram, int wave, uint32_t start, int count, int loop) {
	if (count < 1 || count > 0xffff) throw std::runtime_error("OPL4 wave is too long");
	uint8_t* h = ram.data() + wave * OPL4_HEADER_SIZE;
	uint16_t end = uint16_t(0x10000 - count);
	h[0] = 0x80 | ((start >> 16) & 0x3f); h[1] = start >> 8; h[2] = start;
	h[3] = loop >> 8; h[4] = loop; h[5] = end >> 8; h[6] = end;
	h[7] = 0; h[8] = 0xf0; h[9] = 0; h[10] = 0x0f; h[11] = 0;
}

static Bytes build_ram(const std::vector<Sample>& samples) {
	Bytes ram(OPL4_RAM_SIZE);
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
		for (int16_t v : pcm) { ram[pos++] = uint16_t(v) >> 8; ram[pos++] = v; }
	}
	int noise_wave = samples.size(), lfsr = 1; make_header(ram, noise_wave, OPL4_RAM_ADDRESS + pos, 8192, 0);
	for (int i = 0; i < 8192; ++i) {
		int feedback = (lfsr ^ (lfsr >> 1)) & 1; lfsr = (lfsr >> 1) | (feedback << 14);
		int16_t v = lfsr & 1 ? 12000 : -12000; ram[pos++] = uint16_t(v) >> 8; ram[pos++] = v;
	}
	ram.resize(pos); return ram;
}

static std::vector<Write> trace_spc(const Bytes& data, double seconds) {
	std::vector<Write> writes; trace_target = &writes; trace_clock_base = 0;
	SNES_SPC spc; if (auto e = spc.init()) throw std::runtime_error(e);
	if (auto e = spc.load_spc(data.data(), data.size())) throw std::runtime_error(e);
	std::vector<SNES_SPC::sample_t> audio(2048);
	int pairs_left = int(seconds * SNES_SPC::sample_rate + .5);
	while (pairs_left > 0) {
		int pairs = std::min(pairs_left, 1024);
		if (auto e = spc.play(pairs * 2, audio.data())) throw std::runtime_error(e);
		trace_clock_base += int64_t(pairs) * (SPC_CLOCK / SNES_SPC::sample_rate); pairs_left -= pairs;
	}
	trace_target = nullptr;
	const int64_t final_clock = std::llround(seconds * SPC_CLOCK);
	writes.erase(std::remove_if(writes.begin(), writes.end(), [=](const Write& w) {
		return w.clock < 0 || w.clock > final_clock;
	}), writes.end());
	return writes;
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
static std::pair<int,int> level_pan(const std::array<uint8_t,128>& r, int v, double gain, int minimum) {
	int b = v * 16; double l = std::abs(signed8(r[b]) * signed8(r[0x0c])) / 127.0, rr = std::abs(signed8(r[b+1]) * signed8(r[0x1c])) / 127.0;
	double env = (!(r[b+5] & 0x80) && r[b+7] < 0x80) ? r[b+7] / 127.0 : 1.0, mx = std::max(l, rr), amp = mx / 127.0 * env;
	if (amp <= 0) return {127, 8}; int tl = std::clamp(int(std::lround((-20 * std::log10(std::min(1.0, amp)) - gain) / .375)), minimum, 126);
	static const double p[16][2]={{1,1},{.70795,1},{.50119,1},{.35481,1},{.25119,1},{.17783,1},{.12589,1},{0,1},{0,0},{1,0},{1,.12589},{1,.17783},{1,.25119},{1,.35481},{1,.50119},{1,.70795}};
	double tlr=l/mx,trr=rr/mx,best=1e9; int pan=0; for(int i=0;i<16;++i)if(i!=8){double e=(p[i][0]-tlr)*(p[i][0]-tlr)+(p[i][1]-trr)*(p[i][1]-trr);if(e<best){best=e;pan=i;}}
	return {tl,pan};
}
static void wave_pitch(Bytes& o,int v,int wave,int p){auto [fn,oct]=pitch(p);reg(o,2,0x20+v,((wave>>8)&1)|((fn&127)<<1));reg(o,2,0x38+v,(oct<<4)|((fn>>7)&7));}
static void level(Bytes& o,int v,const std::array<uint8_t,128>& r,double gain,int minimum,bool on){auto [tl,pan]=level_pan(r,v,gain,minimum);reg(o,2,0x50+v,(tl<<1)|1);reg(o,2,0x68+v,pan|(on?0x80:0));}
static void envelope(Bytes&o,int v,const std::array<uint8_t,128>&r){
	int b=v*16,a=r[b+5],d=r[b+6],g=r[b+7],ar,dl,rr=15;
	if(a&0x80){
		double sustain=(double(((d>>5)&7)+1))/8.0;
		int decay_level=std::clamp(int(std::lround(-20.0*std::log10(sustain)/3.0)),0,14);
		ar=((a&15)<<4)|std::min(15,((a>>4)&7)*2+1);
		dl=(decay_level<<4)|std::min(15,int(std::lround((d&31)*15.0/31)));
	}
	else if(g<0x80){ar=0xf0;dl=0;}else{int rate=std::min(15,int(std::lround((g&31)*15.0/31)));if((g>>5)==4||(g>>5)==5){ar=0xf0;dl=rate;}else{ar=rate<<4;dl=0;}}
	reg(o,2,0x98+v,ar);reg(o,2,0xb0+v,dl);reg(o,2,0xc8+v,rr);reg(o,2,0xe0+v,0);
}
static void key_on(Bytes&o,int v,int wave,const std::array<uint8_t,128>&r,double gain,int minimum){
	int b=v*16,p=r[b+2]|((r[b+3]&0x3f)<<8);auto[fn,oct]=pitch(p);reg(o,2,0x20+v,((wave>>8)&1)|((fn&127)<<1));reg(o,2,0x38+v,(oct<<4)|((fn>>7)&7)|((r[0x4d]&(1<<v))?8:0));reg(o,2,8+v,wave);level(o,v,r,gain,minimum,false);reg(o,2,0x80+v,(r[0x2d]&(1<<v))?7:0);envelope(o,v,r);auto q=level_pan(r,v,gain,minimum);reg(o,2,0x68+v,q.second|0x80);
}

struct Tail { Bytes bytes; int total; std::optional<size_t> loop_offset; int loop_sample; };
static Tail playback(const uint8_t*dsp,const std::vector<Sample>& samples,const std::vector<Write>&events,double seconds,double gain,int minimum,int solo,std::optional<double> loop_start){
	std::array<int,128> map;map.fill(-1);for(int i=0;i<int(samples.size());++i)for(int srcn:samples[i].srcn_aliases)map[srcn]=OPL4_RAM_WAVE_BASE+i;
	std::array<uint8_t,128> r{};std::copy(dsp,dsp+128,r.begin());std::array<bool,8> active{};std::array<int,8> aw{};Bytes o;reg(o,1,5,2);reg(o,2,2,0x10);reg(o,2,0xf9,0);
	int noise=OPL4_RAM_WAVE_BASE+samples.size();auto wave_for=[&](int v){return(r[0x3d]&(1<<v))?noise:map[r[v*16+4]];};
	for(int v=0;v<8;++v)if((solo<0||solo==v)&&(dsp[0x4c]&(1<<v))&&!(dsp[0x5c]&(1<<v))&&wave_for(v)>=0){aw[v]=wave_for(v);key_on(o,v,aw[v],r,gain,minimum);active[v]=true;}
	int64_t last=0,ls=loop_start?std::llround(*loop_start*VGM_RATE):0;std::optional<size_t> lo;
	auto wait_to=[&](int64_t target){if(loop_start&&!lo&&last<=ls&&ls<=target){wait(o,ls-last);last=ls;lo=o.size();}if(target>last){wait(o,target-last);last=target;}};
	for(auto e:events){int64_t t=std::llround(e.clock*double(VGM_RATE)/SPC_CLOCK);wait_to(t);r[e.reg]=e.value;
		if(e.reg==0x4c){for(int v=0;v<8;++v)if((e.value&(1<<v))&&(solo<0||solo==v)&&wave_for(v)>=0){aw[v]=wave_for(v);key_on(o,v,aw[v],r,gain,minimum);active[v]=true;}}
		else if(e.reg==0x5c){for(int v=0;v<8;++v)if((e.value&(1<<v))&&(solo<0||solo==v)){reg(o,2,0x68+v,level_pan(r,v,gain,minimum).second);active[v]=false;}}
		else if(e.reg==0x0c||e.reg==0x1c){for(int v=0;v<8;++v)if(active[v]&&(solo<0||solo==v))level(o,v,r,gain,minimum,true);}
		else if(e.reg==0x2d||e.reg==0x3d||e.reg==0x4d){for(int v=0;v<8;++v)if(active[v]&&(solo<0||solo==v)){if(e.reg==0x2d)reg(o,2,0x80+v,(e.value&(1<<v))?7:0);else if(e.reg==0x4d){int b=v*16;auto[fn,oct]=pitch(r[b+2]|((r[b+3]&63)<<8));reg(o,2,0x38+v,(oct<<4)|((fn>>7)&7)|((e.value&(1<<v))?8:0));}else{int w=wave_for(v);if(w>=0&&w!=aw[v]){int b=v*16;wave_pitch(o,v,w,r[b+2]|((r[b+3]&63)<<8));reg(o,2,8+v,w);aw[v]=w;}}}}
		else{int v=e.reg>>4,s=e.reg&15;if(v<8&&active[v]&&(solo<0||solo==v)){if(s==0||s==1||s==5||s==7)level(o,v,r,gain,minimum,true);if(s==5||s==6||s==7)envelope(o,v,r);else if(s==2||s==3){int b=v*16;wave_pitch(o,v,aw[v],r[b+2]|((r[b+3]&63)<<8));}}}
	}
	int total=std::max<int64_t>(last,std::llround(seconds*VGM_RATE));wait_to(total);return{std::move(o),total,lo,int(ls)};
}

static Bytes build_vgm(const Bytes&ram,const Tail&t,double gain){
	Bytes cmd={0x67,0x66,0x84};append32(cmd,8);append32(cmd,OPL4_ROM_SIZE);append32(cmd,0);
	cmd.insert(cmd.end(),{0x67,0x66,0x87});append32(cmd,8+ram.size());append32(cmd,OPL4_RAM_SIZE);append32(cmd,0);cmd.insert(cmd.end(),ram.begin(),ram.end());size_t tail=cmd.size();cmd.insert(cmd.end(),t.bytes.begin(),t.bytes.end());cmd.push_back(0x66);
	Bytes v(VGM_HEADER_SIZE);std::copy_n("Vgm ",4,v.begin());put32(v,8,0x171);put32(v,0x18,t.total);put32(v,0x24,60);put32(v,0x34,VGM_HEADER_SIZE-0x34);put32(v,0x60,OPL4_CLOCK);v[0x7c]=uint8_t(std::clamp(int(std::lround(gain*16/6)), -64,192));
	if(t.loop_offset){uint32_t absolute=VGM_HEADER_SIZE+tail+*t.loop_offset;put32(v,0x1c,absolute-0x1c);put32(v,0x20,t.total-t.loop_sample);}
	v.insert(v.end(),cmd.begin(),cmd.end());put32(v,4,v.size()-4);return v;
}
static void manifest(const fs::path&p,const std::vector<Sample>&s){
	fs::create_directories(p.parent_path());std::ofstream o(p);o<<"wave,srcn,srcn_aliases,start_hex,loop_hex,brr_bytes,pcm_samples,loop_sample,looped\n";
	for(int i=0;i<int(s.size());++i){o<<i<<','<<s[i].index<<',';for(size_t a=0;a<s[i].srcn_aliases.size();++a){if(a)o<<'|';o<<s[i].srcn_aliases[a];}o<<",0x"<<std::hex<<std::uppercase<<std::setw(4)<<std::setfill('0')<<s[i].start<<",0x"<<std::setw(4)<<s[i].loop<<std::dec<<','<<s[i].brr.size()<<','<<s[i].pcm.size()<<','<<s[i].loop_sample<<','<<s[i].looped<<'\n';}
}
static void convert(const fs::path&in,const fs::path&out,const fs::path&csv,const Options&opt){
	Bytes d=read_file(in);if(d.size()<SPC_DSP_OFFSET+SPC_DSP_SIZE||std::memcmp(d.data(),"SNES-SPC700 Sound File Data",25))throw std::runtime_error("invalid SPC: "+in.string());
	auto samples=extract_samples(d.data()+SPC_RAM_OFFSET,d.data()+SPC_DSP_OFFSET,opt.max_samples);auto ram=build_ram(samples);Timing ti=timing(d,opt.fallback);double seconds=opt.playback>=0?opt.playback:ti.duration;auto events=trace_spc(d,seconds);auto loop=ti.loop_start;
	if(!loop&&!opt.no_loop_detect)if(auto found=detect_loop(events,d.data()+SPC_DSP_OFFSET)){loop=found->first;seconds=found->second;}
	const int64_t playback_end = std::llround(seconds * SPC_CLOCK);
	events.erase(std::remove_if(events.begin(), events.end(), [=](const Write& w) {
		return w.clock > playback_end;
	}), events.end());
	Tail tail=playback(d.data()+SPC_DSP_OFFSET,samples,events,seconds,opt.hardware_gain,opt.minimum_tl,opt.solo_voice,loop);write_file(out,build_vgm(ram,tail,opt.header_gain));if(!csv.empty())manifest(csv,samples);
	std::cout<<"wrote "<<out<<" ("<<samples.size()<<" samples, "<<ram.size()<<" OPL4 RAM bytes, "<<seconds<<" seconds";
	if(loop)std::cout<<", loop "<<*loop<<"s";std::cout<<")\n";
}
static void usage(const char*n){std::cerr<<"usage: "<<n<<" input.spc [-o output.vgm] [--manifest file.csv] [--auto-playback]\n       "<<n<<" --batch DIR [--batch-output DIR] [--manifest-output DIR]\n"; }
static Options parse(int ac,char**av){
	Options o;for(int i=1;i<ac;++i){std::string a=av[i];auto value=[&](){if(++i>=ac)throw std::runtime_error("missing value after "+a);return std::string(av[i]);};
		if(a=="-o"||a=="--output")o.output=value();else if(a=="--manifest")o.manifest=value();else if(a=="--batch")o.batch=value();else if(a=="--batch-output")o.batch_output=value();else if(a=="--manifest-output")o.manifest_output=value();else if(a=="--playback")o.playback=std::stod(value());else if(a=="--fallback-seconds")o.fallback=std::stod(value());else if(a=="--playback-gain-db")o.header_gain=std::stod(value());else if(a=="--hardware-gain-db")o.hardware_gain=std::stod(value());else if(a=="--minimum-tl")o.minimum_tl=std::stoi(value());else if(a=="--max-samples")o.max_samples=std::stoi(value());else if(a=="--solo-voice")o.solo_voice=std::stoi(value());else if(a=="--auto-playback")o.auto_playback=true;else if(a=="--no-loop-detect")o.no_loop_detect=true;else if(a=="-h"||a=="--help"){usage(av[0]);std::exit(0);}else if(a[0]=='-')throw std::runtime_error("unknown option "+a);else o.input=a;
	}return o;
}
int main(int ac,char**av){try{Options o=parse(ac,av);if(!o.batch.empty()){fs::path out=o.batch_output.empty()?o.batch/"vgm":o.batch_output;std::vector<fs::path> files;for(auto&e:fs::directory_iterator(o.batch))if(e.path().extension()==".spc")files.push_back(e.path());std::sort(files.begin(),files.end());for(auto&p:files){fs::path csv=o.manifest_output.empty()?fs::path{}:o.manifest_output/(p.stem().string()+".csv");convert(p,out/(p.stem().string()+".vgm"),csv,o);}return 0;}if(o.input.empty()){usage(av[0]);return 2;}if(o.output.empty()){o.output=o.input;o.output.replace_extension(".vgm");}convert(o.input,o.output,o.manifest,o);return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
