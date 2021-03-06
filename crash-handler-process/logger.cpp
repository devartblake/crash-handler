/******************************************************************************
	Copyright (C) 2016-2020 by Streamlabs (General Workings Inc)

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.

******************************************************************************/

#include "logger.hpp"

#include <ctime>
#include <time.h>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <sys/types.h>
#include <stdlib.h>
#if defined(WIN32)
#include <filesystem>
#include <process.h>
#else // for __APPLE__ and other 
#include <unistd.h>
#endif

bool log_output_disabled = false;

#ifdef WIN32
namespace fs = std::filesystem;
#endif
std::ofstream log_output_file;
static int pid;

const std::string getTimeStamp()
{
	time_t t = time(NULL);
	struct tm buf;
#if defined(WIN32)
	localtime_s(&buf, &t);
#else  // for __APPLE__ and other 
	localtime_r(&t, &buf);
#endif 	

	char mbstr[64] = {0};
	std::strftime(mbstr, sizeof(mbstr), "%Y%m%d:%H%M%S.", &buf);
	uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

	std::ostringstream ss;
	ss << pid << ":" << mbstr << std::setw(3) << std::setfill('0') << now % 1000;
	return ss.str();
}

void logging_start(std::wstring log_path)
{
#ifdef __APPLE__
	log_output_disabled = false;
	return;
#endif
	if (log_path.size() == 0)
		log_output_disabled = true;

	if (!log_output_disabled)
	{
#if defined(WIN32)
		pid = _getpid();
#else  // for __APPLE__ and other 
		pid = getpid();
#endif 	
#ifdef WIN32
		try
		{
			std::uintmax_t size = std::filesystem::file_size(log_path);
			if (size > 1 * 1024 * 1024)
			{
				fs::path log_file = log_path;
				fs::path log_file_old = log_file;
				log_file_old.replace_extension("log.old");

				fs::remove_all(log_file_old);
				fs::rename(log_file, log_file_old);
			}
		}
		catch (...)
		{
		}
		log_output_file.open(log_path, std::ios_base::out | std::ios_base::app);
#endif
	}
}

void logging_end()
{
#ifdef WIN32
	if (!log_output_disabled)
		log_output_file.close();
#endif
}