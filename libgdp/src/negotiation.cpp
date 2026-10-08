// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "gdp/negotiation.hpp"

#include <algorithm>

namespace gdp {

namespace {

bool contains(const std::vector<std::string> &list, const std::string &name) {
	return std::find(list.begin(), list.end(), name) != list.end();
}

// `offered` filtered to what `supported` also has, in `offered`'s order,
// duplicates dropped: the one rule behind both negotiations.
std::vector<std::string> intersect_in_order(const std::vector<std::string> &offered,
	const std::vector<std::string> &supported) {
	std::vector<std::string> common;
	for (const auto &name : offered) {
		if (contains(supported, name) && !contains(common, name)) {
			common.push_back(name);
		}
	}
	return common;
}

} // namespace

std::vector<std::string> common_video_codecs(const std::vector<std::string> &offered,
	const std::vector<std::string> &supported) {
	return intersect_in_order(offered, supported);
}

std::vector<std::string> negotiate_capabilities(const std::vector<std::string> &offered,
	const std::vector<std::string> &supported) {
	return intersect_in_order(offered, supported);
}

bool has_capability(const std::vector<std::string> &negotiated, const std::string &name) {
	return contains(negotiated, name);
}

} // namespace gdp
