#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "SNES_SPC.h"

struct DspWrite {
	long long time;
	int reg;
	int value;
};

static std::vector<DspWrite> writes;
static long long clock_base;

void spc_trace_dsp_write(int time, int reg, int value)
{
	if ((reg & 0x80) == 0)
		writes.push_back({clock_base + time, reg & 0x7F, value & 0xFF});
}

static std::vector<unsigned char> read_file(const std::string& path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file)
		throw std::runtime_error("unable to open input file");
	return std::vector<unsigned char>(
		std::istreambuf_iterator<char>(file),
		std::istreambuf_iterator<char>());
}

int main(int argc, char** argv)
{
	if (argc < 2 || argc > 3) {
		std::cerr << "usage: " << argv[0] << " input.spc [seconds]\n";
		return 2;
	}

	const double seconds = argc == 3 ? std::atof(argv[2]) : 60.0;
	if (seconds <= 0.0) {
		std::cerr << "seconds must be positive\n";
		return 2;
	}

	try {
		const std::vector<unsigned char> data = read_file(argv[1]);
		SNES_SPC spc;
		if (const char* error = spc.init())
			throw std::runtime_error(error);
		if (const char* error = spc.load_spc(data.data(), static_cast<long>(data.size())))
			throw std::runtime_error(error);

		std::vector<SNES_SPC::sample_t> audio(2048);
		int pairs_remaining = static_cast<int>(seconds * SNES_SPC::sample_rate + 0.5);
		while (pairs_remaining > 0) {
			const int pairs = std::min(pairs_remaining, 1024);
			if (const char* error = spc.play(pairs * 2, audio.data()))
				throw std::runtime_error(error);
			clock_base += static_cast<long long>(pairs) * (1024000 / SNES_SPC::sample_rate);
			pairs_remaining -= pairs;
		}

		std::cout << "clock,reg,value\n";
		for (const DspWrite& write : writes)
			std::cout << write.time << ',' << write.reg << ',' << write.value << '\n';
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
	return 0;
}
