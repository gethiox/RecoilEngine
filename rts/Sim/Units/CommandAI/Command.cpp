/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Command.h"
#include "System/creg/Serializer.h"
#include "CommandParamsPool.hpp"

CommandParamsPool cmdParamsPool;

CR_BIND(Command, )
CR_REG_METADATA(Command, (
	CR_MEMBER(id),

	CR_MEMBER(timeOut),
	CR_IGNORED(pageIndex),
	CR_MEMBER(numParams),
	CR_MEMBER(tag),
	CR_MEMBER(options),

	CR_IGNORED(params),
	CR_SERIALIZER(Serialize)
))

Command::~Command() {
	if (!IsPooledCommand())
		return;

	cmdParamsPool.ReleasePage(pageIndex);
}


const float* Command::GetParams(unsigned int idx) const {
	if (idx >= numParams)
		return nullptr;

	if (IsPooledCommand()) {
		assert(numParams > MAX_COMMAND_PARAMS);
		return (cmdParamsPool.GetPtr(pageIndex, idx));
	}

	return ((idx < MAX_COMMAND_PARAMS)? &params[idx]: nullptr);
}

float Command::GetParam(unsigned int idx) const {
	const float* ptr = GetParams(idx);

	if (ptr != nullptr)
		return *ptr;

	return 0.0f;
}


bool Command::SetParam(unsigned int idx, float param) {
	float* ptr = const_cast<float*>(GetParams(idx));

	if (ptr != nullptr)
		return (*ptr = param, true);

	return false;
}

bool Command::PushParam(float param) {
	if (numParams < MAX_COMMAND_PARAMS) {
		// no need to make this a pooled command just yet
		params[numParams++] = param;
		return true;
	}

	if (!IsPooledCommand()) {
		// not in the pool, reserve an entry and fill it
		pageIndex = cmdParamsPool.AcquirePage();

		for (unsigned int i = 0; i < numParams; i++) {
			cmdParamsPool.Push(pageIndex, params[i]);
		}

		memset(&params[0], 0, sizeof(params));
		assert(IsPooledCommand());
	}

	// add new parameter
	numParams = cmdParamsPool.Push(pageIndex, param);
	return true;
}

void Command::CopyParams(const Command& c) {
	// clear existing params
	if (IsPooledCommand())
		cmdParamsPool.ReleasePage(pageIndex);

	pageIndex = -1u;
	numParams = 0;

	assert(IsEmptyCommand());

	for (unsigned int i = 0; i < c.numParams; i++) {
		PushParam(c.GetParam(i));
	}
}

void Command::Serialize(creg::ISerializer* s) {
	if (s->IsWriting()) {
		// stored as they are, also the key of a timed wait: it is the same on
		// every client for waits issued through the network, and a loaded
		// client is then one of them. (The owning client replaces the key of a
		// factory-built unit's wait by a local one, CWaitCommandsAI::
		// AddLocalUnit; its snapshot differs from the others' then and the
		// join falls back, which is the safe outcome: a canonical 0 would
		// have made the joiner the only client with that value, visible to
		// Lua through the command queues.)
		for (unsigned int i = 0; i < numParams; i++) {
			float p = GetParam(i);
			s->Serialize(&p, sizeof(p));
		}
	} else {
		const unsigned int tempNumParams = numParams;
		for (numParams = 0; numParams < tempNumParams;) {
			float p;
			s->Serialize(&p, sizeof(p));
			PushParam(p);
		}
	}
}
