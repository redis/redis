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
#include <assert.h>

#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <string>
#include <sstream>


class Numastat
{
public:
	//Returns numastat memory usage per node.
	static float get_total_memory(unsigned node)
	{
		static pid_t pid = getpid();

		std::stringstream cmd;
		cmd << "numastat " << pid;

		FILE* file;
		char buff[256];
		float result = -1.0;

		if((file = popen(cmd.str().c_str(), "r")))
		{
			while(fgets(buff, 256, file));
			pclose(file);

			//We got: "Total                     1181.90            2.00         1183.90".
			//2.00 is our memory usage from Node 1.
			std::string last_line(buff);

			size_t dot_pos = 0;
			//Now we search in: "           2.00         1183.90".
			for(unsigned i=0; i<=node; i++)
			{
				dot_pos = last_line.find(".", dot_pos+1);
				assert(dot_pos != std::string::npos);
			}

			//We are at: " 2.00         1183.90".
			size_t number_begin = last_line.rfind(" ", dot_pos);
			assert(number_begin != std::string::npos);

			number_begin += 1;
			buff[dot_pos+3] = '\0';

#if __cplusplus > 201100L
			result = strtod(&buff[number_begin], NULL);
#else
			result = atof(&buff[number_begin]);
#endif
		}

		assert(result > -0.01);

		return result;
	}

};