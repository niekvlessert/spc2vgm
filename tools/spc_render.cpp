#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "SNES_SPC.h"

void spc_trace_dsp_write(int, int, int) {}

static std::vector<unsigned char> read_file(const std::string& path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file)
		throw std::runtime_error("unable to open input file");
	return std::vector<unsigned char>(
		std::istreambuf_iterator<char>(file),
		std::istreambuf_iterator<char>());
}

static void write_u16(std::ofstream& file, uint16_t value)
{
	file.put(value & 0xFF);
	file.put(value >> 8);
}

static void write_u32(std::ofstream& file, uint32_t value)
{
	write_u16(file, value & 0xFFFF);
	write_u16(file, value >> 16);
}

int main(int argc, char** argv)
{
	if (argc < 4 || argc > 5) {
		std::cerr << "usage: " << argv[0] << " input.spc seconds output.wav [solo_voice]\n";
		return 2;
	}

	try {
		const std::vector<unsigned char> data = read_file(argv[1]);
		const int pairs = static_cast<int>(std::stod(argv[2]) * SNES_SPC::sample_rate + 0.5);
		std::vector<SNES_SPC::sample_t> audio(pairs * 2);
		SNES_SPC spc;
		if (const char* error = spc.init())
			throw std::runtime_error(error);
		if (const char* error = spc.load_spc(data.data(), static_cast<long>(data.size())))
			throw std::runtime_error(error);
		if (argc == 5) {
			const int voice = std::stoi(argv[4]);
			if (voice < 0 || voice >= 8)
				throw std::runtime_error("solo_voice must be 0..7");
			spc.mute_voices(0xFF & ~(1 << voice));
		}
		if (const char* error = spc.play(static_cast<int>(audio.size()), audio.data()))
			throw std::runtime_error(error);

		std::ofstream out(argv[3], std::ios::binary);
		const uint32_t audio_bytes = static_cast<uint32_t>(audio.size() * sizeof(audio[0]));
		out.write("RIFF", 4);
		write_u32(out, 36 + audio_bytes);
		out.write("WAVEfmt ", 8);
		write_u32(out, 16);
		write_u16(out, 1);
		write_u16(out, 2);
		write_u32(out, SNES_SPC::sample_rate);
		write_u32(out, SNES_SPC::sample_rate * 4);
		write_u16(out, 4);
		write_u16(out, 16);
		out.write("data", 4);
		write_u32(out, audio_bytes);
		out.write(reinterpret_cast<const char*>(audio.data()), audio_bytes);
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
	return 0;
}
