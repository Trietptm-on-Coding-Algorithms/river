#include "X86Assembler.h"

#include "mm.h"

#define FLAG_GENERATE_RIVER_xSP		0x0F
/*#define FLAG_GENERATE_RIVER_xSP_xAX	0x01
#define FLAG_GENERATE_RIVER_xSP_xCX	0x02
#define FLAG_GENERATE_RIVER_xSP_xDX	0x04
#define FLAG_GENERATE_RIVER_xSP_xBX	0x08*/

#define FLAG_SKIP_METAOP			0x10
//#define FLAG_GENERATE_RIVER			0x20

static void FixRiverEspOp(BYTE opType, RiverOperand *op, BYTE repReg) {
	switch (RIVER_OPTYPE(opType)) {
	case RIVER_OPTYPE_IMM:
	case RIVER_OPTYPE_NONE:
		break;
	case RIVER_OPTYPE_REG:
		if (op->asRegister.name == RIVER_REG_xSP) {
			op->asRegister.versioned = repReg;
		}
		break;
	case RIVER_OPTYPE_MEM:
		break;
	default:
		__asm int 3;
	}
}

const RiverInstruction *FixRiverEspInstruction(const RiverInstruction &rIn, RiverInstruction *rTmp, RiverAddress *aTmp) {
	if (rIn.family & RIVER_FAMILY_FLAG_ORIG_xSP) {
		BYTE repReg = rIn.GetUnusedRegister();

		memcpy(rTmp, &rIn, sizeof(*rTmp));
		for (BYTE i = 0; i < 4; ++i) {
			if (rIn.opTypes[i] != RIVER_OPTYPE_NONE) {
				if (RIVER_OPTYPE(rIn.opTypes[i]) == RIVER_OPTYPE_MEM) {
					rTmp->operands[i].asAddress = aTmp;
					memcpy(aTmp, rIn.operands[i].asAddress, sizeof(*aTmp));
				}
				FixRiverEspOp(rTmp->opTypes[i], &rTmp->operands[i], repReg);
			}
		}

		return rTmp;
	}
	else {
		return &rIn;
	}
}

void X86Assembler::SwitchToRiver(BYTE *&px86, DWORD &instrCounter) {
	static const unsigned char code[] = { 0x87, 0x25, 0x00, 0x00, 0x00, 0x00 };			// 0x00 - xchg esp, large ds:<dwVirtualStack>}

	memcpy(px86, code, sizeof(code));
	*(unsigned int *)(&(px86[0x02])) = (unsigned int)&runtime->execBuff;

	px86 += sizeof(code);
	instrCounter++;
}

void X86Assembler::SwitchToRiverEsp(BYTE *&px86, DWORD &instrCounter, BYTE repReg) {
	static const unsigned char code[] = { 0x87, 0x05, 0x00, 0x00, 0x00, 0x00 };			// 0x00 - xchg eax, large ds:<dwVirtualStack>}

	memcpy(px86, code, sizeof(code));
	px86[0x01] |= (repReg & 0x03); // choose the acording replacement register
	*(unsigned int *)(&(px86[0x02])) = (unsigned int)&runtime->execBuff;

	px86 += sizeof(code);
	instrCounter++;
}

void X86Assembler::EndRiverConversion(RelocableCodeBuffer &px86, DWORD &pFlags, BYTE &currentFamily, BYTE &repReg, DWORD &instrCounter) {
	if (RIVER_FAMILY_NATIVE != currentFamily) {
		DWORD currentStack;

		switch (currentFamily)	{
			case RIVER_FAMILY_NATIVE:
				break;
			case RIVER_FAMILY_RIVER:
				currentStack = (DWORD)&runtime->execBuff;
				break;
			case RIVER_FAMILY_PRETRACK:
				currentStack = (DWORD)&runtime->trackBuff;
				break;
			default:
				__asm int 3;
				break;
		}

		SwitchToNative(px86, currentFamily, repReg, instrCounter, currentStack);
	}
}

bool X86Assembler::Translate(const RiverInstruction &ri, RelocableCodeBuffer &px86, DWORD &pFlags, BYTE &currentFamily, BYTE &repReg, DWORD &instrCounter) {
	// skip ignored instructions
	if (ri.family & RIVER_FAMILY_FLAG_IGNORE) {
		return true;
	}

	//if (/*(RIVER_FAMILY(ri.family) == RIVER_FAMILY_TRACK) || */(RIVER_FAMILY(ri.family) == RIVER_FAMILY_PRETRACK)) {
	//	return true;
	//}

	if (RIVER_FAMILY_RIVER == RIVER_FAMILY(ri.family)) {
		return true;
	}

	// when generating fwcode skip meta instructions
	if ((RIVER_FAMILY(ri.family) == RIVER_FAMILY_PREMETAOP) || (RIVER_FAMILY(ri.family) == RIVER_FAMILY_POSTMETAOP)) {
		if (pFlags & FLAG_SKIP_METAOP) {
			return true;
		}
	}

	GenerateTransitions(ri, px86, pFlags, currentFamily, repReg, instrCounter);
	GeneratePrefixes(ri, px86.cursor);

	RiverInstruction rInstr;
	RiverAddress32 rAddr;

	const RiverInstruction *rOut = FixRiverEspInstruction(ri, &rInstr, &rAddr);

	DWORD dwTable = 0;

	/*if (rOut->modifiers & RIVER_MODIFIER_EXT) {
		*px86.cursor = 0x0F;
		px86.cursor++;
		dwTable = 1;
	}*/

	//(this->*assembleOpcodes[dwTable][rOut->opCode])(*rOut, px86, pFlags, instrCounter);
	//(this->*assembleOperands[dwTable][rOut->opCode])(*rOut, px86);

	GenericX86Assembler *casm = &nAsm;

	if (RIVER_FAMILY(rOut->family) == RIVER_FAMILY_RIVER) {
		casm = &rAsm;
	} else if (RIVER_FAMILY(rOut->family) == RIVER_FAMILY_TRACK) {
		casm = &tAsm; 
	} else if (RIVER_FAMILY(rOut->family) == RIVER_FAMILY_PRETRACK) {
		casm = &ptAsm; 
	}

	casm->Translate(*rOut, px86, pFlags, currentFamily, repReg, instrCounter);


	return true;
}


bool X86Assembler::Assemble(RiverInstruction *pRiver, DWORD dwInstrCount, RelocableCodeBuffer &px86, DWORD flg, DWORD &instrCounter, DWORD &byteCounter) {
	BYTE *pTmp = px86.cursor, *pAux = px86.cursor;
	DWORD pFlags = flg;
	BYTE currentFamily = RIVER_FAMILY_NATIVE;
	BYTE repReg = 0;

	DbgPrint("= river to x86 ================================================================\n");

	for (DWORD i = 0; i < dwInstrCount; ++i) {
		pTmp = px86.cursor;

		//pFlags = flg;
		if (!Translate(pRiver[i], px86, pFlags, currentFamily, repReg, instrCounter)) {
			return false;
		}
		//ConvertRiverInstruction(cg, rt, &pRiver[i], &pTmp, &pFlags);

		for (; pTmp < px86.cursor; ++pTmp) {
			DbgPrint("%02x ", *pTmp);
		}
		DbgPrint("\n");
	}

	EndRiverConversion(px86, pFlags, currentFamily, repReg, instrCounter);
	for (; pTmp < px86.cursor; ++pTmp) {
		DbgPrint("%02x ", *pTmp);
	}
	DbgPrint("\n");

	DbgPrint("===============================================================================\n");
	byteCounter = px86.cursor - pAux;
	return true;
}

void X86Assembler::SwitchEspWithReg(RelocableCodeBuffer &px86, DWORD &instrCounter, BYTE repReg, DWORD dwStack) {
	static const BYTE code[] = { 0x87, 0x05, 0x00, 0x00, 0x00, 0x00 };			// 0x00 - xchg eax, large ds:<dwVirtualStack>}

	memcpy(px86.cursor, code, sizeof(code));
	px86.cursor[0x01] |= (repReg & 0x03); // choose the acording replacement register
	*(DWORD *)(&px86.cursor[0x02]) = dwStack;

	px86.cursor += sizeof(code);
	instrCounter++;
}

void X86Assembler::SwitchToStack(RelocableCodeBuffer &px86, DWORD &instrCounter, DWORD dwStack) {
	static const unsigned char code[] = { 0x87, 0x25, 0x00, 0x00, 0x00, 0x00 };			// 0x00 - xchg esp, large ds:<dwVirtualStack>}

	memcpy(px86.cursor, code, sizeof(code));
	*(DWORD *)(&px86.cursor[0x02]) = dwStack;

	px86.cursor += sizeof(code);
	instrCounter++;
}

bool X86Assembler::SwitchToNative(RelocableCodeBuffer &px86, BYTE &currentFamily, BYTE repReg, DWORD &instrCounter, DWORD dwStack) {
	if (currentFamily & RIVER_FAMILY_FLAG_ORIG_xSP) {
		SwitchEspWithReg(px86, instrCounter, repReg, dwStack);
		currentFamily &= ~RIVER_FAMILY_FLAG_ORIG_xSP;
	}

	switch (RIVER_FAMILY(currentFamily)) {
		case RIVER_FAMILY_NATIVE : 
			break;
		case RIVER_FAMILY_RIVER :
			SwitchToStack(px86, instrCounter, (DWORD)&runtime->execBuff);
			break;
		case RIVER_FAMILY_PRETRACK :
			SwitchToStack(px86, instrCounter, (DWORD)&runtime->trackBuff);
			break;
		default:
			__asm int 3;
			break;
	}

	currentFamily = RIVER_FAMILY_NATIVE;
	return true;
}

bool X86Assembler::GenerateTransitions(const RiverInstruction &ri, RelocableCodeBuffer &px86, DWORD &pFlags, BYTE &currentFamily, BYTE &repReg, DWORD &instrCounter) {
	BYTE cf = RIVER_FAMILY(currentFamily);
	BYTE tf = RIVER_FAMILY(ri.family);

	DWORD currentStack = 0;

	switch (cf)	{
		case RIVER_FAMILY_NATIVE:
			break;
		case RIVER_FAMILY_RIVER:
			currentStack = (DWORD)&runtime->execBuff;
			break;
		case RIVER_FAMILY_PRETRACK:
			currentStack = (DWORD)&runtime->trackBuff;
			break;
		default:
			__asm int 3;
			break;
	}

	if (cf != tf) {
		SwitchToNative(px86, currentFamily, repReg, instrCounter, currentStack);
		cf = RIVER_FAMILY(currentFamily);
	}

	if (cf != tf) {
		switch (tf) {
			case RIVER_FAMILY_NATIVE:
				break;
			case RIVER_FAMILY_RIVER:
				SwitchToStack(px86, instrCounter, (DWORD)&runtime->execBuff);
				currentFamily = RIVER_FAMILY_RIVER;
				currentStack = (DWORD)&runtime->execBuff;
				break;
			case RIVER_FAMILY_PRETRACK:
				SwitchToStack(px86, instrCounter, (DWORD)&runtime->trackBuff);
				currentFamily = RIVER_FAMILY_PRETRACK;
				currentStack = (DWORD)&runtime->trackBuff;
				break;
			default:
				__asm int 3;
				break;
		}
	}

	// now both current and target families have the same value
	if (RIVER_FAMILY_FLAG_ORIG_xSP & currentFamily) { // current family preservers ESP
		if (RIVER_FAMILY_FLAG_ORIG_xSP & ri.family) { // target family preservers ESP
			if (0 == ((1 << repReg) & ri.unusedRegisters)) { // switch repReg
				SwitchEspWithReg(px86, instrCounter, repReg, currentStack);
				repReg = ri.GetUnusedRegister();
				SwitchEspWithReg(px86, instrCounter, repReg, currentStack);
			}
		} else {
			SwitchEspWithReg(px86, instrCounter, repReg, currentStack);
			currentFamily &= ~RIVER_FAMILY_FLAG_ORIG_xSP;
		}
	} else { // current family doesn't preserve ESP
		if (RIVER_FAMILY_FLAG_ORIG_xSP & ri.family) {
			SwitchEspWithReg(px86, instrCounter, repReg, currentStack);
			currentFamily |= RIVER_FAMILY_FLAG_ORIG_xSP;
		}
	}

	return true;
}

/*bool X86Assembler::GenerateTransitions(const RiverInstruction &ri, BYTE *&px86, DWORD &pFlags, BYTE &repReg, DWORD &instrCounter) {
	//FIXME skip all symbop instructions
	if (RIVER_FAMILY(ri.family) == RIVER_FAMILY_PRETRACK) {
		return true;
	}

	if (RIVER_FAMILY(ri.family) == RIVER_FAMILY_TRACK) {
		return true;
	}

	// ensure state transitions between river and x86
	if (RIVER_FAMILY(ri.family) == RIVER_FAMILY_RIVER) { // current instr is river
		if (0 == (pFlags & FLAG_GENERATE_RIVER)) {
			SwitchToRiver(px86, instrCounter);
			pFlags |= FLAG_GENERATE_RIVER;
		}

		// ensure state transitions between riveresp and river
		if (ri.family & RIVER_FAMILY_FLAG_ORIG_xSP) { // ri needs riveresp
			if (0 == (pFlags & FLAG_GENERATE_RIVER_xSP)) {
				repReg = ri.GetUnusedRegister();
				SwitchToRiverEsp(px86, instrCounter, repReg);
				pFlags |= FLAG_GENERATE_RIVER_xSP;
			}
			else {
				if (0 == ((1 << repReg) & ri.unusedRegisters)) { // switch repReg
					SwitchToRiverEsp(px86, instrCounter, repReg);
					repReg = ri.GetUnusedRegister();
					SwitchToRiverEsp(px86, instrCounter, repReg);
				}
			}
		}
		else {
			if (pFlags & FLAG_GENERATE_RIVER_xSP) {
				SwitchToRiverEsp(px86, instrCounter, repReg);
				pFlags &= ~FLAG_GENERATE_RIVER_xSP;
			}
		}
	}
	else { // current instr is x86
		if (pFlags & FLAG_GENERATE_RIVER) {
			if (pFlags & FLAG_GENERATE_RIVER_xSP) {
				SwitchToRiverEsp(px86, instrCounter, repReg);
				repReg = 0;
				pFlags &= ~FLAG_GENERATE_RIVER_xSP;
			}

			SwitchToRiver(px86, instrCounter);
			pFlags &= ~FLAG_GENERATE_RIVER;
		}
	}

	return true;
}*/

bool X86Assembler::Init(RiverRuntime *runtime) {
	this->runtime = runtime;

	nAsm.Init(runtime);
	rAsm.Init(runtime);
	ptAsm.Init(runtime);
	tAsm.Init(runtime);

	return true;
}
