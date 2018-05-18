/*
* Copyright (C) 2015 - 2016 Intel Corporation.
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
* 1. Redistributions of source code must retain the above copyright notice(s),
*    this list of conditions and the following disclaimer.
* 2. Redistributions in binary form must reproduce the above copyright notice(s),
*    this list of conditions and the following disclaimer in the documentation
*    and/or other materials provided with the distribution.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER(S) ``AS IS'' AND ANY EXPRESS
* OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO
* EVENT SHALL THE COPYRIGHT HOLDER(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
* LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
* PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
* LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
* OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
* ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#pragma once

#include <map>
#include <vector>
#include <string>

class CommandLine
{
public:

	//Parse and write to val when option exist and strtol(...) > 0, otherwise val is not changed.
	//T should be an integer type.
	template<class T>
	void parse_with_strtol(const std::string& option, T& val)
	{
		if(args.count(option))
		{
			T tmp = static_cast<T>(strtol(args[option].c_str(), NULL, 10));
			if(tmp > 0)
				val = tmp;
			else
				printf("Warning! Option '%s' may not be set.\n", option.c_str());
		}
		else
		{
			//Do not modify val.
			printf("Warning! Option '%s' is not present.\n", option.c_str());
		}
	}

	bool is_option_present(const std::string& option) const
	{
		return args.count(option);
	}

	bool is_option_set(const std::string option, std::string val)
	{
		if(is_option_present(option))
			return (args[option] == val);
		return false;
	}

	const std::string& get_option_value(const std::string& option)
	{
		return args[option];
	}

	CommandLine(int argc, char* argv[])
	{
		for (int i=0; i<argc; i++)
		{
			std::string arg(argv[i]);
			size_t found = arg.find("=");

			if(found != std::string::npos)
			{
				std::string option = arg.substr(0, found);
				std::string val = arg.substr(found+1, arg.length());

				args[option] = val;
			}
		}
	}

private:
	std::map<std::string, std::string> args;
};