#include "stdafx.h"
#include "resource_watcher.hpp"
#include "utils.hpp"

using std::vector;

namespace {
	struct RawSection {
		RawSection(const string &name, const char *start, size_t len) : name(name), start(start), len(len) {}
		string name;
		const char *start;
		size_t len;
	};

	const char *next_row(const char *buf, const char **pos, string *row)
	{
		const char *start = buf;
		char ch = *buf;
		while (ch != '\n' && ch != '\r' && ch != '\0')
			ch = *++buf;

		*row = string(start, buf - start);
		*pos = start;

		while (ch == '\r' || ch == '\n')
			ch = *++buf;

		return ch == '\0' ? NULL : buf;
	}

	void parse_sections(const char *buf, size_t len, vector<RawSection> *sections)
	{
		// section header "---[[[ HEADER NAME ]]]---"
		string cur;
		const char *last_section = NULL;
		const char *next = buf;
		string header;
		do {
			const char *pos;
			next = next_row(next, &pos, &cur);
			const char *start = strstr(cur.c_str(), "---[[[");
			const char *end = strstr(cur.c_str(), "]]]---");
			if (start && end) {
				// save the previous section
				if (last_section) 
					sections->push_back(RawSection(header, last_section, pos - last_section));

				int ofs = start - cur.c_str() + 6;
				const char *p = &cur.data()[ofs];
				header = trim(cur.substr(ofs, end - p));
				last_section = next;
			}
		} while (next);

		if (!header.empty())
			sections->push_back(RawSection(header, last_section, &buf[len] - last_section));
	}

	uint32_t fnv_hash(const char *p, size_t len)
	{
		const uint32_t FNV32_prime = 16777619UL;
		const uint32_t FNV32_init  = 2166136261UL;

		static uint32_t h = FNV32_init;

		for (size_t i = 0; i < len; i++) {
			h = h * FNV32_prime;
			h = h ^ p[i];
		}

		return h;
	}
}

void extract_sections(const char *buf, size_t len, vector<Section> *sections)
{
	vector<RawSection> raw_sections;
	parse_sections(buf, len, &raw_sections);
	for (size_t i = 0; i < raw_sections.size(); ++i) {
		const RawSection &raw = raw_sections[i];
		sections->push_back(Section(raw.name, string(raw.start, raw.len)));

	}
}

