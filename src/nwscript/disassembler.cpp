/* xoreos-tools - Tools to help with xoreos development
 *
 * xoreos-tools is the legal property of its developers, whose names
 * can be found in the AUTHORS file distributed with this source
 * distribution.
 *
 * xoreos-tools is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 * xoreos-tools is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with xoreos-tools. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file
 *  Disassembling NWScript bytecode.
 */

#include <cassert>

#include "src/common/strutil.h"
#include "src/common/maths.h"
#include "src/common/readstream.h"
#include "src/common/writestream.h"

#include "src/nwscript/disassembler.h"
#include "src/nwscript/ncsfile.h"
#include "src/nwscript/util.h"
#include "src/nwscript/game.h"
#include "disassembler.h"


namespace NWScript {

static Common::UString quoteString(const Common::UString &str) {
	Common::UString out;

	for (Common::UString::iterator c = str.begin(); c != str.end(); ++c) {
		if      (*c == '\\')
			out += "\\\\";
		else if (*c == '"')
			out += "\\\"";
		else
			out += *c;
	}

	return out;
}


Disassembler::Disassembler(Common::SeekableReadStream &ncs, Aurora::GameID game) {
	_ncs.reset(new NCSFile(ncs, game));
}

Disassembler::Disassembler(NCSFile *ncs) : _ncs(ncs) {
}

Disassembler::~Disassembler() {
}

void Disassembler::analyzeStack() {
	_ncs->analyzeStack();
}

void Disassembler::analyzeControlFlow() {
	_ncs->analyzeControlFlow();
}

void Disassembler::createListing(Common::WriteStream &out, bool printStack) {
	writeInfo(out);
	writeEngineTypes(out);

	const Instructions &instr = _ncs->getInstructions();

	for (Instructions::const_iterator i = instr.begin(); i != instr.end(); ++i) {
		writeJumpLabel(out, *i);

		if (_ncs->hasStackAnalysis() && printStack)
			writeStack(out, *i, 36);

		// Print the actual disassembly line
		out.writeString(Common::UString::format("  %08X %-26s %s\n", i->address,
			              formatBytes(*i).c_str(), formatInstruction(*i, _ncs->getGame()).c_str()));

		// If this instruction has no natural follower, print a separator
		if (!i->follower)
			out.writeString("  -------- -------------------------- ---\n");
	}
}

void Disassembler::createAssembly(Common::WriteStream &out, bool printStack) {
	writeInfo(out);
	writeEngineTypes(out);

	const Instructions &instr = _ncs->getInstructions();

	for (Instructions::const_iterator i = instr.begin(); i != instr.end(); ++i) {
		writeJumpLabel(out, *i);

		if (_ncs->hasStackAnalysis() && printStack)
			writeStack(out, *i, 0);

		// Print the actual disassembly line
		out.writeString(Common::UString::format("  %s\n", formatInstruction(*i, _ncs->getGame()).c_str()));

		// If this instruction has no natural follower, print an empty line as separator
		if (!i->follower)
			out.writeString("\n");
	}
}

void Disassembler::createDot(Common::WriteStream &out, bool printControlTypes) {
	/* This creates a GraphViz dot file, which can be drawn into a graph image
	 * with graphviz's dot tool.
	 *
	 * Each block of NWScript instructions is drawn into one (or several, for large blocks)
	 * node, clustered by subroutine. Edges are drawn between the nodes to show the control
	 * flow.
	 */

	out.writeString("digraph {\n");
	out.writeString("  overlap=false\n");
	out.writeString("  concentrate=true\n");
	out.writeString("  splines=ortho\n\n");

	writeDotClusteredBlocks(out, printControlTypes);
	writeDotBlockEdges     (out);

	out.writeString("}\n");
}

void Disassembler::createNSS(Common::WriteStream &out) {
	out.writeString("// Decompiled using ncsdis");

	const Stack &stack = _ncs->getGlobals();

	out.writeString("\n\n");
	for (const auto &global : stack) {
		out.writeString(getVariableTypeName(global.variable->type, _ncs->getGame()));
		out.writeString(" " + formatVariableName(global.variable));
		out.writeString(Common::composeString(global.variable->id));
		out.writeString("\n");
	}

	const SubRoutines &subRoutines = _ncs->getSubRoutines();

	for (const auto &subRoutine : subRoutines) {
		writeNSSSubRoutine(out, subRoutine);
	}
}

void Disassembler::writeDotClusteredBlocks(Common::WriteStream &out, bool printControlTypes) {
	const SubRoutines &subs = _ncs->getSubRoutines();

	// Block nodes grouped into subroutines clusters
	for (SubRoutines::const_iterator s = subs.begin(); s != subs.end(); ++s) {
		if (s->blocks.empty() || s->blocks.front()->instructions.empty())
			continue;

		out.writeString(Common::UString::format(
		                "  subgraph cluster_s%08X {\n"
		                "    style=filled\n"
		                "    color=lightgrey\n", s->address));

		Common::UString clusterLabel = getSignature(*s);
		if (clusterLabel.empty())
			clusterLabel = formatJumpLabelName(*s);
		if (clusterLabel.empty())
			clusterLabel = formatJumpDestination(s->address);

		out.writeString(Common::UString::format("    label=\"%s\"\n\n", clusterLabel.c_str()));

		writeDotBlocks(out, printControlTypes, s->blocks);

		out.writeString("  }\n\n");
	}
}

static size_t calculateNodesPerBlock(size_t blockSize) {
	// Max number of instructions per node
	static const size_t kMaxNodeSize = 10;

	return ceil(blockSize / (double)kMaxNodeSize);
}

static Common::UString getBlockControl(const Block &block) {
	Common::UString control;

	for (std::vector<ControlStructure>::const_iterator c = block.controls.begin();
	     c != block.controls.end(); ++c) {

		switch (c->type) {
			case kControlTypeNone:
				control += "<NONE>";
				break;
			case kControlTypeDoWhileHead:
				control += "<DOWHILEHEAD>";
				break;
			case kControlTypeDoWhileTail:
				control += "<DOWHILETAIL>";
				break;
			case kControlTypeDoWhileNext:
				control += "<DOWHILENEXT>";
				break;
			case kControlTypeWhileHead:
				control += "<WHILEHEAD>";
				break;
			case kControlTypeWhileTail:
				control += "<WHILETAIL>";
				break;
			case kControlTypeWhileNext:
				control += "<WHILENEXT>";
				break;
			case kControlTypeBreak:
				control += "<BREAK>";
				break;
			case kControlTypeContinue:
				control += "<CONTINUE>";
				break;
			case kControlTypeReturn:
				control += "<RETURN>";
				break;
			case kControlTypeIfCond:
				control += "<IFCOND>";
				break;
			case kControlTypeIfTrue:
				control += "<IFTRUE>";
				break;
			case kControlTypeIfElse:
				control += "<IFELSE>";
				break;
			case kControlTypeIfNext:
				control += "<IFNEXT>";
				break;
			default:
				control += "<>";
				break;
		}

		control += "\\n";
	}

	if (!control.empty())
		control += "\\n";

	return control;
}

void Disassembler::writeDotBlocks(Common::WriteStream &out, bool printControlTypes,
                                  const std::vector<const Block *> &blocks) {

	for (std::vector<const Block *>::const_iterator b = blocks.begin(); b != blocks.end(); ++b) {
		/* To keep large nodes from messing up the layout, we divide blocks with
		 * a huge amount of instructions into several, equal-sized nodes. */

		const size_t nodeCount = calculateNodesPerBlock((*b)->instructions.size());

		std::vector<Common::UString> labels;
		labels.resize(nodeCount);

		const size_t linesPerNode = ceil((*b)->instructions.size() / (double)labels.size());

		Common::UString control;
		if (printControlTypes)
			control = getBlockControl(**b);

		labels[0] = formatJumpLabelName(**b);
		if (labels[0].empty())
			labels[0] = formatJumpDestination((*b)->instructions.front()->address);

		labels[0] += ":\\l";

		labels[0] = control + labels[0];

		// Instructions
		for (size_t i = 0; i < (*b)->instructions.size(); i++) {
			const Instruction &instr = *(*b)->instructions[i];

			labels[i / linesPerNode] += "  " + quoteString(formatInstruction(instr, _ncs->getGame())) + "\\l";
		}

		// Nodes
		for (size_t i = 0; i < labels.size(); i++) {
			const Common::UString n    = Common::composeString(i);
			const Common::UString name = Common::UString::format("b%08X_%s", (*b)->address, n.c_str());

			out.writeString(Common::UString::format("    \"%s\" ", name.c_str()));
			out.writeString("[ shape=\"box\" label=\"" + labels[i] + "\" ]\n");
		}

		// Edges between the divided block nodes
		if (labels.size() > 1) {
			for (size_t i = 0; i < labels.size(); i++) {
				const Common::UString n = Common::composeString(i);

				out.writeString((i == 0) ? "    " : " -> ");
				out.writeString(Common::UString::format("b%08X_%s", (*b)->address, n.c_str()));
			}
			out.writeString(" [ style=dotted ]\n");
		}

		if (b != --blocks.end())
			out.writeString("\n");
	}
}

void Disassembler::writeDotBlockEdges(Common::WriteStream &out) {
	const Blocks &blocks = _ncs->getBlocks();

	for (Blocks::const_iterator b = blocks.begin(); b != blocks.end(); ++b) {
		assert(b->children.size() == b->childrenTypes.size());

		for (size_t i = 0; i < b->children.size(); i++) {
			const size_t lastIndex = calculateNodesPerBlock(b->instructions.size()) - 1;

			out.writeString(Common::UString::format("  b%08X_%s -> b%08X_0", b->address,
			                Common::composeString(lastIndex).c_str(), b->children[i]->address));

			Common::UString attr;

			// Color the edge specific to the flow type
			switch (b->childrenTypes[i]) {
				default:
				case kBlockEdgeTypeUnconditional:
					attr = "color=blue";
					break;

				case kBlockEdgeTypeConditionalTrue:
					attr = "color=green";
					break;

				case kBlockEdgeTypeConditionalFalse:
					attr = "color=red";
					break;

				case kBlockEdgeTypeSubRoutineCall:
					attr = "color=cyan";
					break;

				case kBlockEdgeTypeSubRoutineTail:
					attr = "color=orange";
					break;

				case kBlockEdgeTypeSubRoutineStore:
					attr = "color=purple";
					break;

				case kBlockEdgeTypeDead:
					attr = "color=gray40";
					break;
			}

			// If this is a jump back, make the edge bold
			if (b->children[i]->address < b->address)
				attr += " style=bold";

			// If this edge goes between subroutines, don't let the edge influence the node rank
			if (b->subRoutine != b->children[i]->subRoutine)
				attr += " constraint=false";

			out.writeString(" [ " + attr + " ]\n");
		}
	}
}

void Disassembler::writeInfo(Common::WriteStream &out) {
	out.writeString(Common::UString::format("; %s bytes, %s instructions\n\n",
	                Common::composeString(_ncs->size()).c_str(),
	                Common::composeString(_ncs->getInstructions().size()).c_str()));
}

void Disassembler::writeEngineTypes(Common::WriteStream &out) {
	size_t engineTypeCount = getEngineTypeCount(_ncs->getGame());
	if (engineTypeCount > 0) {
		out.writeString("; Engine types:\n");

		for (size_t i = 0; i < engineTypeCount; i++) {
			const Common::UString name = getEngineTypeName(_ncs->getGame(), i);
			if (name.empty())
				continue;

			const Common::UString gName = getGenericEngineTypeName(i);

			out.writeString(Common::UString::format("; %s: %s\n", gName.c_str(), name.c_str()));
		}

		out.writeString("\n");
	}
}

void Disassembler::writeJumpLabel(Common::WriteStream &out, const Instruction &instr) {
	Common::UString jumpLabel = formatJumpLabelName(instr);
	if (!jumpLabel.empty()) {
		jumpLabel += ":";

		const Common::UString signature = getSignature(instr);
		if (!signature.empty())
			jumpLabel += " ; " + signature;
	}

	if (!jumpLabel.empty())
		out.writeString(jumpLabel + "\n");
}

void Disassembler::writeStack(Common::WriteStream &out, const Instruction &instr, size_t indent) {
	const Common::UString stackSize = Common::composeString(instr.stack.size());

	out.writeString(Common::UString(' ', indent));
	out.writeString(Common::UString::format("; .--- Stack: %4s ---\n", stackSize.c_str()));

	for (size_t s = 0; s < instr.stack.size(); s++) {
		const Variable &var = *instr.stack[s].variable;

		Common::UString siblings;
		for (std::set<const Variable *>::const_iterator sib = var.siblings.begin();
		     sib != var.siblings.end(); ++sib) {

			if (!siblings.empty())
				siblings += ",";

			siblings += Common::composeString((*sib)->id);
		}

		if (!siblings.empty())
			siblings = " (" + siblings + ")";

		const Common::UString stackIndex = Common::composeString(s);
		const Common::UString stackID    = Common::composeString(var.id);

		out.writeString(Common::UString(' ', indent));
		out.writeString(Common::UString::format("; | %4s - %6s: %-8s (%08X)%s\n",
		    stackIndex.c_str(), stackID.c_str(),
		    getVariableTypeName(var.type, _ncs->getGame()).toLower().c_str(),
		    var.creator ? var.creator->address : 0, siblings.c_str()));
	}

	out.writeString(Common::UString(' ', indent));
	out.writeString("; '--- ---------- ---\n");
}

Common::UString Disassembler::getSignature(const SubRoutine &sub) {
	if (!_ncs->hasStackAnalysis())
		return "";

	if ((sub.type == kSubRoutineTypeStart) || (sub.type == kSubRoutineTypeGlobal) ||
	    (sub.type == kSubRoutineTypeStoreState))
		return "";

	if (sub.stackAnalyzeState != kStackAnalyzeStateFinished)
		return "";

	return formatSignature(sub);
}

Common::UString Disassembler::getSignature(const Instruction &instr) {
	if (!_ncs->hasStackAnalysis())
		return "";

	if ((instr.addressType != kAddressTypeSubRoutine) || !instr.block || !instr.block->subRoutine)
		return "";

	return getSignature(*instr.block->subRoutine);
}

void Disassembler::writeNSSSubRoutine(Common::WriteStream &out, const SubRoutine &subRoutine) {
	out.writeString("\n\n");
	out.writeString(formatSignature(subRoutine, _ncs->getGame(), true));
	out.writeString(" {\n");

	assert(subRoutine.returns.size() <=1);

	writeNSSBlock(out, subRoutine.blocks[0], 1);

	out.writeString("}");
}

void Disassembler::writeNSSBlock(Common::WriteStream &out, const Block *block, size_t indent) {
	for (const auto instruction : block->instructions) {
		writeNSSInstruction(out, instruction, indent);
	}

	VariableSpace variables = _ncs->getVariables();

	for (const auto &childType : block->childrenTypes) {
		if (isSubRoutineCall(childType)) {
			writeNSSIndent(out, indent);
			const Instruction *instruction = block->instructions.back();


			out.writeString(formatJumpLabelName(*instruction->branches[0]));
			out.writeString("(");

			for (size_t i = 0; i < instruction->variables.size(); ++i) {
				out.writeString(formatVariableName(instruction->variables[i]));
				if (i < instruction->variables.size() - 1)
					out.writeString(", ");
			}

			out.writeString(");\n");

			writeNSSBlock(out, block->children[1], indent);
		}
	}

	for (const auto &control : block->controls) {
		if (control.type == kControlTypeReturn) {
			writeNSSIndent(out, indent);

			if (control.retn->instructions.size() > 0) {
				if (control.retn->instructions.back()->stack.size() == 0) {
					out.writeString("return;\n");
					continue;
				}

				out.writeString("return ");

				out.writeString(formatVariableName(control.retn->instructions[0]->variables[0]));

				out.writeString(";\n");
			} else {
				out.writeString("return;\n");
			}
		} else if (control.type == kControlTypeIfCond) {
			writeNSSIfBlock(out, control, indent);
		}

		// TODO: While loop
	}
}

void Disassembler::writeNSSIfBlock(Common::WriteStream &out, const ControlStructure &control, size_t indent) {
	writeNSSIndent(out, indent);

	const Variable *cond = control.ifCond->instructions.back()->variables[0];
	out.writeString("if (");
	out.writeString(formatVariableName(cond));
	out.writeString(") {\n");

	if (control.ifTrue)
		writeNSSBlock(out, control.ifTrue, indent + 1);

	writeNSSIndent(out, indent);
	out.writeString("}");

	if (control.ifElse) {
		out.writeString(" else {\n");
		writeNSSBlock(out, control.ifElse, indent + 1);

		writeNSSIndent(out, indent);
		out.writeString("}");
	}
	out.writeString("\n");

	if (control.ifNext)
		writeNSSBlock(out, control.ifNext, indent);
}

void Disassembler::writeNSSInstruction(Common::WriteStream &out, const Instruction *instruction, size_t indent) {
	switch (instruction->opcode) {
		case kOpcodeCONST: {
			const Variable *v = instruction->variables[0];
			writeNSSIndent(out, indent);
			out.writeString(getVariableTypeName(v->type) + " " + formatVariableName(v) + " = " + formatInstructionData(*instruction) + ";\n");

			break;
		}

		case kOpcodeACTION: {
			unsigned int paramCount = instruction->args[1];

			writeNSSIndent(out, indent);

			if (instruction->variables.size() > paramCount) {
				const Variable *ret = instruction->variables.back();
				out.writeString(getVariableTypeName(ret->type, _ncs->getGame()) + " " + formatVariableName(ret) + " = ");
			}

			out.writeString(getFunctionName(_ncs->getGame(), instruction->args[0]));
			out.writeString("(");
			for (unsigned int i = 0; i < paramCount; ++i) {
				out.writeString(formatVariableName(instruction->variables[i]));
				if (i < paramCount - 1)
					out.writeString(", ");
			}
			out.writeString(");\n");

			break;
		}

		case kOpcodeCPDOWNBP:
		case kOpcodeCPDOWNSP:
		case kOpcodeCPTOPBP:
		case kOpcodeCPTOPSP: {
			const Variable *v1 = instruction->variables[0];
			const Variable *v2 = instruction->variables[1];

			writeNSSIndent(out, indent);
			out.writeString(getVariableTypeName(v2->type, _ncs->getGame()) + " " + formatVariableName(v2) + " = " + formatVariableName(v1) + ";\n");

			break;
		}

		case kOpcodeLOGAND: {
			const Variable *v1 = instruction->variables[0];
			const Variable *v2 = instruction->variables[1];
			const Variable *result = instruction->variables[2];

			writeNSSIndent(out, indent);
			out.writeString(
					getVariableTypeName(result->type, _ncs->getGame()) + " " +
					formatVariableName(result) + " = " +
					formatVariableName(v1) + " && " + formatVariableName(v2) + ";\n"
			);

			break;
		}

		case kOpcodeLOGOR: {
			const Variable *v1 = instruction->variables[0];
			const Variable *v2 = instruction->variables[1];
			const Variable *result = instruction->variables[2];

			writeNSSIndent(out, indent);
			out.writeString(
					getVariableTypeName(result->type, _ncs->getGame()) + " " +
					formatVariableName(result) + " = " +
					formatVariableName(v1) + " || " + formatVariableName(v2) + ";\n"
			);

			break;
		}

		case kOpcodeEQ: {
			const Variable *v1 = instruction->variables[0];
			const Variable *v2 = instruction->variables[1];
			const Variable *result = instruction->variables[2];

			writeNSSIndent(out, indent);
			out.writeString(
					getVariableTypeName(result->type, _ncs->getGame()) + " " +
					formatVariableName(result) + " = " +
					formatVariableName(v1) + " == " + formatVariableName(v2) + ";\n"
			);


			break;
		}

		case kOpcodeLEQ: {
			const Variable *v1 = instruction->variables[0];
			const Variable *v2 = instruction->variables[1];
			const Variable *result = instruction->variables[2];

			writeNSSIndent(out, indent);
			out.writeString(
					getVariableTypeName(result->type, _ncs->getGame()) + " " +
					formatVariableName(result) + " = " +
					formatVariableName(v1) + " <= " + formatVariableName(v2) + ";\n"
			);

			break;
		}

		case kOpcodeLT: {
			const Variable *v1 = instruction->variables[0];
			const Variable *v2 = instruction->variables[1];
			const Variable *result = instruction->variables[2];

			writeNSSIndent(out, indent);
			out.writeString(
					getVariableTypeName(result->type, _ncs->getGame()) + " " +
					formatVariableName(result) + " = " +
					formatVariableName(v1) + " < " + formatVariableName(v2) + ";\n"
			);

			break;
		}

		case kOpcodeGEQ: {
			const Variable *v1 = instruction->variables[0];
			const Variable *v2 = instruction->variables[1];
			const Variable *result = instruction->variables[2];

			writeNSSIndent(out, indent);
			out.writeString(
					getVariableTypeName(result->type, _ncs->getGame()) + " " +
					formatVariableName(result) + " = " +
					formatVariableName(v1) + " >= " + formatVariableName(v2) + ";\n"
			);

			break;
		}

		case kOpcodeGT: {
			const Variable *v1 = instruction->variables[0];
			const Variable *v2 = instruction->variables[1];
			const Variable *result = instruction->variables[2];

			writeNSSIndent(out, indent);
			out.writeString(
					getVariableTypeName(result->type, _ncs->getGame()) + " " +
					formatVariableName(result) + " = " +
					formatVariableName(v1) + " > " + formatVariableName(v2) + ";\n"
			);

			break;
		}

		case kOpcodeNOT: {
			const Variable *v = instruction->variables[0];
			const Variable *result = instruction->variables[2];

			writeNSSIndent(out, indent);
			out.writeString(
					getVariableTypeName(result->type, _ncs->getGame()) + " " +
					formatVariableName(result) + " = " +
					"!" + formatVariableName(v) + ";\n"
			);

			break;
		}

		case kOpcodeRSADD: {
			const Variable *v = instruction->variables[0];

			writeNSSIndent(out, indent);
			out.writeString(
					getVariableTypeName(v->type, _ncs->getGame()) + " " +
					formatVariableName(v) + " = "
			);

			switch (v->type) {
				case kTypeString:
					out.writeString("\"\"");
					break;
				case kTypeInt:
					out.writeString("0");
					break;
				case kTypeFloat:
					out.writeString("0.0");
					break;

				default:
					// TODO: No idea how empty objects or engine types are intialized.
					out.writeString("0");
					break;
			}

			out.writeString(";\n");

			break;
		}

		// TODO: Not all necessary instruction are implemented here

		default:
			break;
	}
}

void Disassembler::writeNSSIndent(Common::WriteStream &out, size_t indent) {
	for (size_t i = 0; i < indent; ++i)
		out.writeString("\t");
}

} // End of namespace NWScript
