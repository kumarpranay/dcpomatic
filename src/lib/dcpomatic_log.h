/*
    Copyright (C) 2018 Carl Hetherington <cth@carlh.net>

    This file is part of DCP-o-matic.

    DCP-o-matic is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    DCP-o-matic is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with DCP-o-matic.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "log.h"
#include "compose.hpp"
#include <boost/shared_ptr.hpp>

/** The current log; set up by the front-ends when they have a Film to log into */
extern boost::shared_ptr<Log> dcpomatic_log;

#define LOG_GENERAL(...)      dcpomatic_log->log(String::compose(__VA_ARGS__), LogEntry::TYPE_GENERAL);
#define LOG_GENERAL_NC(...)   dcpomatic_log->log(__VA_ARGS__, LogEntry::TYPE_GENERAL);
#define LOG_ERROR(...)        dcpomatic_log->log(String::compose(__VA_ARGS__), LogEntry::TYPE_ERROR);
#define LOG_ERROR_NC(...)     dcpomatic_log->log(__VA_ARGS__, LogEntry::TYPE_ERROR);
#define LOG_WARNING(...)      dcpomatic_log->log(String::compose(__VA_ARGS__), LogEntry::TYPE_WARNING);
#define LOG_WARNING_NC(...)   dcpomatic_log->log(__VA_ARGS__, LogEntry::TYPE_WARNING);
#define LOG_TIMING(...)       dcpomatic_log->log(String::compose(__VA_ARGS__), LogEntry::TYPE_TIMING);
#define LOG_DEBUG_ENCODE(...) dcpomatic_log->log(String::compose(__VA_ARGS__), LogEntry::TYPE_DEBUG_ENCODE);
#define LOG_DEBUG_PLAYER(...) dcpomatic_log->log(String::compose(__VA_ARGS__), LogEntry::TYPE_DEBUG_PLAYER);
