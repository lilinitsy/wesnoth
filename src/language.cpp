/* vim:set encoding=utf-8: */
/*
   Copyright (C) 2003 - 2008 by David White <dave@whitevine.net>
   Part of the Battle for Wesnoth Project http://www.wesnoth.org/

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 2
   or at your option any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/

#include "global.hpp"

#include "config.hpp"
#include "filesystem.hpp"
#include "foreach.hpp"
#include "game_config.hpp"
#include "gettext.hpp"
#include "language.hpp"
#include "log.hpp"
#include "preferences.hpp"
#include "util.hpp"
#include "serialization/parser.hpp"
#include "serialization/preprocessor.hpp"
#include "wesconfig.h" //without this DUMMYLOCALES break

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>

#ifdef _WIN32
//#include "locale.h"
#include <windows.h>
extern "C" int _putenv(const char*);
#endif

#define DBG_FS LOG_STREAM(debug, filesystem)

/** Tests one locale to be available. */
static bool has_locale(const char* s) {
	try {
		// The way to find out whether a locale is available is to set it and
		// hope not runtime error gets thrown.
		std::locale dummy(s);
		return true;
	} catch (std::runtime_error&) {
		return false;
	}
}		

/** Test the locale for a language and it's utf-8 variations. */
static bool has_language(const std::string& language)
{
	if(has_locale(language.c_str())) {
		return true;
	}

	std::string utf = language + ".utf-8";
	if(has_locale(utf.c_str())) {
		return true;
	}

	utf = language + ".UTF-8";
	if(has_locale(utf.c_str())) {
		return true;
	}
	
	return false;
}

namespace {
	language_def current_language;
	string_map strings_;
}

static std::vector<language_def> known_languages;

std::string languagedef_name (const language_def& def)
{
	return def.language;
}

bool current_language_rtl()
{
	return get_language().rtl;
}

bool language_def::operator== (const language_def& a) const
{
	return ((language == a.language) /* && (localename == a.localename) */ );
}

bool language_def::available() const
{
#ifdef USE_DUMMYLOCALES
	// Dummy has every language available.
	return true;
#else
	if(has_language(localename)) {
		return true;
	} else {
		foreach(const std::string& lang, alternates) {
			if(has_language(lang)) {
				return true;
			}
		}
	}
	return false;
#endif
}

symbol_table string_table;

const t_string& symbol_table::operator[](const std::string& key) const
{
	const string_map::const_iterator i = strings_.find(key);
	if(i != strings_.end()) {
		return i->second;
	} else {
		static t_string empty_string;
		// Let's do it the painful way (untlb means untranslatABLE).
		// It will cause problem if somebody stores more than one reference at once
		// but I don't really care since this path is an error path and it should
		// not have been taken in the first place. -- silene
		empty_string = "UNTLB " + key;
		return empty_string;
	}
}

const t_string& symbol_table::operator[](const char* key) const
{
	return (*this)[std::string(key)];
}

bool load_language_list()
{
	config cfg;
	try {
		scoped_istream stream = preprocess_file("data/hardwired/language.cfg");
		read(cfg, *stream);
	} catch(config::error &) {
		return false;
	}

	known_languages.clear();
	known_languages.push_back(
		language_def("", t_string(N_("System default language"), "wesnoth"), "ltr"));

	config::const_child_itors langs = cfg.child_range("locale");
	for(;langs.first != langs.second; ++langs.first) {
		known_languages.push_back(
			language_def((**langs.first)["locale"], (**langs.first)["name"], (**langs.first)["dir"],
				(**langs.first)["alternates"]));
	}

	return true;
}

std::vector<language_def> get_languages()
{
	return known_languages;
}

static void wesnoth_setlocale(int category, std::string const &slocale,
	std::vector<std::string> const *alternates)
{
	char const *locale = slocale.c_str();
	// FIXME: ideally we should check LANGUAGE and on first invocation
	// use that value, so someone with es would get the game in Spanish
	// instead of en_US the first time round
	// LANGUAGE overrides other settings, so for now just get rid of it
	// FIXME: add configure check for unsetenv
#ifndef _WIN32
#ifndef __AMIGAOS4__
	unsetenv ("LANGUAGE"); // void so no return value to check
#endif
#endif

#ifdef __BEOS__
	if(setenv ("LANG", locale, 1) == -1)
		std::cerr << "setenv LANG failed: " << strerror(errno);
	if(setenv ("LC_ALL", locale, 1) == -1)
		std::cerr << "setenv LC_ALL failed: " << strerror(errno);
#endif
#ifdef __APPLE__
	if(setenv ("LANGUAGE", locale, 1) == -1)
		std::cerr << "setenv LANGUAGE failed: " << strerror(errno);
	if(setenv ("LC_ALL", locale, 1) == -1)
		std::cerr << "setenv LC_ALL failed: " << strerror(errno);
#endif

#ifdef _WIN32
	std::string env = "LANG=" + slocale;
	_putenv(env.c_str());
	env = "LC_ALL=" + slocale;
	_putenv(env.c_str());
	std::string win_locale = locale;
	win_locale = win_locale.substr(0,2);
	#include "language_win32.ii"
	SetEnvironmentVariable("LANG", win_locale.c_str());
	SetEnvironmentVariable("LC_ALL", win_locale.c_str());
	if(category == LC_MESSAGES)
	    category = LC_ALL;
#endif

#ifdef USE_DUMMYLOCALES
	static enum { UNINIT, NONE, PRESENT } status = UNINIT;
	static std::string locpath;
	if (status == UNINIT) {
		if (char const *p = getenv("LOCPATH")) {
			locpath = p;
			status = PRESENT;
		} else status = NONE;
	}
	if (slocale.empty())
		if (status == NONE)
			unsetenv("LOCPATH");
		else
			setenv("LOCPATH", locpath.c_str(), 1);
	else setenv("LOCPATH", (game_config::path + "/locales").c_str(), 1);
	std::string xlocale;
	if (!slocale.empty()) {
		// dummy suffix to prevent locale aliasing from kicking in
		xlocale = slocale + "@wesnoth";
		locale = xlocale.c_str();
	}
#endif

	char *res = NULL;
	#ifdef _WIN32
	    char const *try_loc = win_locale.c_str();
	#else
		char const *try_loc = locale;
	#endif
	std::vector<std::string>::const_iterator i;
	if (alternates) i = alternates->begin();
	while (true) {
		res = std::setlocale(category, try_loc);
		if (res) break;

		std::string utf8 = std::string(try_loc) + std::string(".utf-8");
		res = std::setlocale(category, utf8.c_str());
		if (res) break;

		utf8 = std::string(try_loc) + std::string(".UTF-8");
		res = std::setlocale(category, utf8.c_str());
		if (res) break;

		if (!alternates) break;
		if (i == alternates->end()) break;
		try_loc = i->c_str();
		i++;
	}

	if (res == NULL)
		std::cerr << "WARNING: setlocale() failed for '"
			  << locale << "'.\n";
	else
		std::cerr << "set locale to '" << try_loc << "'\n";
}

bool set_language(const language_def& locale)
{
	strings_.clear();

	std::string locale_lc;
	locale_lc.resize(locale.localename.size());
	std::transform(locale.localename.begin(),locale.localename.end(),locale_lc.begin(),tolower);

	config cfg;

	current_language = locale;
	wesnoth_setlocale(LC_MESSAGES, locale.localename, &locale.alternates);
	wesnoth_setlocale(LC_COLLATE, locale.localename, &locale.alternates);

	// fill string_table (should be moved somwhere else some day)
	try {
		scoped_istream stream = preprocess_file("data/hardwired/english.cfg");
		read(cfg, *stream);
	} catch(config::error& e) {
		std::cerr << "Could not read english.cfg\n";
		throw e;
	}

	config* langp = cfg.child("language");
	if (langp == NULL) {
		std::cerr << "No [language] block found in english.cfg\n";
		return false;
	}

	for(string_map::const_iterator j = langp->values.begin(); j != langp->values.end(); ++j) {
		strings_[j->first] = j->second;
	}
	// end of string_table fill

	// Reset translations for the name of current languages
	for (std::vector<language_def>::iterator itor = known_languages.begin();
			itor != known_languages.end(); ++itor) {

		itor->language.reset_translation();
	}

	return true;
}

const language_def& get_language() { return current_language; }

const language_def& get_locale()
{
	//TODO: Add in support for querying the locale on Windows

	assert(known_languages.size() != 0);

	const std::string& prefs_locale = preferences::language();
	if(prefs_locale.empty() == false) {
		wesnoth_setlocale(LC_MESSAGES, prefs_locale, NULL);
		for(std::vector<language_def>::const_iterator i = known_languages.begin();
				i != known_languages.end(); ++i) {
			if (prefs_locale == i->localename)
				return *i;
		}
		LOG_STREAM(info, general) << "'" << prefs_locale << "' locale not found in known array; defaulting to system locale\n";
		return known_languages[0];
	}

#if 0
	const char* const locale = getenv("LANG");
	#ifdef _WIN32
	    std::string win_locale = locale
		#include "language_win32.ii"
		return win_locale;
	#endif
	if(locale != NULL && strlen(locale) >= 2) {
		//we can't pass pointers into the string to the std::string
		//constructor because some STL implementations don't support
		//it (*cough* MSVC++6)
		std::string res(2,'z');
		res[0] = tolower(locale[0]);
		res[1] = tolower(locale[1]);
		return res;
	}
#endif

	LOG_STREAM(info, general) << "locale could not be determined; defaulting to system locale\n";
	return known_languages[0];
}

void init_textdomains(const config& cfg)
{
	config::const_child_itors t = cfg.child_range("textdomain");

	for(;t.first != t.second; ++t.first) {
		const std::string name = (**t.first)["name"];
		const std::string path = (**t.first)["path"];

		if(path.empty()) {
			t_string::add_textdomain(name, get_intl_dir());
		} else {
			//This is adapted version from a call to get_binary_file_location()
			DBG_FS << "  Looking for textdomain path" << path << "\n";
			std::string location;

			const std::vector<std::string>& paths = get_binary_paths(path);
			for(std::vector<std::string>::const_iterator i = paths.begin(); i != paths.end(); ++i) {
				DBG_FS << "  Checking " << *i << "\n";
				if(is_directory(*i + path)) {
					location = *i + path;
					break;
				}
			}

			if (location.empty()) {
				//if location is empty, this causes a crash on Windows, so we
				//disallow adding empty domains
				std::cerr << "no location found for '" << path << "', not adding textdomain\n";
			} else {
				DBG_FS << "  Found at " << location << "\n";
				t_string::add_textdomain(name, location);
			} 
		}
	}

}

