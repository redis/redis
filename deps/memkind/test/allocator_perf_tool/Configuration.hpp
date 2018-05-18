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

#include <string>
#include <map>
#include <assert.h>

//AllocatorTypes class represent allocator types and names related to this types.
class AllocatorTypes
{
public:
	enum
	{
		STANDARD_ALLOCATOR,
		JEMALLOC,
		MEMKIND_DEFAULT,
		MEMKIND_HBW,
		MEMKIND_INTERLEAVE,
		MEMKIND_HBW_INTERLEAVE,
		MEMKIND_HBW_PREFERRED,
		MEMKIND_HUGETLB,
		MEMKIND_GBTLB,
		MEMKIND_HBW_HUGETLB,
		MEMKIND_HBW_PREFERRED_HUGETLB,
		MEMKIND_HBW_GBTLB,
		MEMKIND_HBW_PREFERRED_GBTLB,
		NUM_OF_ALLOCATOR_TYPES
	};

	static const std::string& allocator_name(unsigned type)
	{
		static const std::string names[] =
		{
			"STANDARD_ALLOCATOR",
			"JEMALLOC",
			"MEMKIND_DEFAULT",
			"MEMKIND_HBW",
			"MEMKIND_INTERLEAVE",
			"MEMKIND_HBW_INTERLEAVE",
			"MEMKIND_HBW_PREFERRED",
			"MEMKIND_HUGETLB",
			"MEMKIND_GBTLB",
			"MEMKIND_HBW_HUGETLB",
			"MEMKIND_HBW_PREFERRED_HUGETLB",
			"MEMKIND_HBW_GBTLB",
			"MEMKIND_HBW_PREFERRED_GBTLB"
		};

		if(type >= NUM_OF_ALLOCATOR_TYPES) assert(!"Invalid input argument!");

		return names[type];
	}

	static unsigned allocator_type(const std::string& name)
	{
		for (unsigned i=0; i<NUM_OF_ALLOCATOR_TYPES; i++)
		{
			if(allocator_name(i) == name)
				return i;
		}

		assert(!"Invalid input argument!");
	}

	static bool is_valid_memkind(unsigned type)
	{
		return (type >= MEMKIND_DEFAULT) && (type < NUM_OF_ALLOCATOR_TYPES);
	}
};

//Enable or disable enum values (types).
class TypesConf
{
public:
	TypesConf() {}

	TypesConf(unsigned type)
	{
		enable_type(type);
	}

	void enable_type(unsigned type) {types[type] = true;}

	void disable_type(unsigned type)
	{
		if(types.count(type))
			types[type] = false;
	}

	bool is_enabled(unsigned type) {return (types.count(type) ? types[type] : false);}

private:
	std::map<unsigned, bool> types;
};

//AllocationSizesConf class represents allocation sizes configuration.
//This data is needed to generate "n" sizes in range from "size_from" to "size_to".
class AllocationSizesConf
{
public:
	unsigned n;
	unsigned reserved_unallocated; // limit allocations
	size_t size_from;
	size_t size_to;
};

//TaskConf class contain configuration data for task,
//where:
// - "n" - number of iterations,
// - "allocation_sizes_conf" - allocation sizes configuration,
// - "func_calls" - enabled or disabled function calls,
// - "allocators_types" - enable allocators,
// - "seed" - random seed.
class TaskConf
{
public:
	unsigned n;
	AllocationSizesConf allocation_sizes_conf;
	TypesConf func_calls;
	TypesConf allocators_types;
	unsigned seed;
	bool is_csv_log_enabled;
	bool check_memory_availability;
};