/*
 * Copyright 2018-2019 TON DEV SOLUTIONS LTD.
 *
 * Licensed under the  terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License.
 *
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the  GNU General Public License for more details at: https://www.gnu.org/licenses/gpl-3.0.html
 */
/**
 * @author TON Labs <connect@tonlabs.io>
 * @date 2019
 * Common TVM codegen routines, in particular, types, data structures, scope, stack manipulations, etc.
 */

#pragma once

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

#include <liblangutil/ErrorReporter.h>
#include <liblangutil/SourceReferenceFormatter.h>

#include <functional>
#include <typeinfo>
#include <json/json.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <utility>

#include "TVMConstants.hpp"

using namespace std;
using namespace dev;
using namespace dev::solidity;
using namespace langutil;

#define DBG(x) cout << x << endl;

namespace dev {
namespace solidity {
	
template <typename T>	using string_map	= std::map<std::string, T>;
template <typename T>	using ptr_vec		= std::vector<ASTPointer<T>>;

template <typename T1, typename T2>
T1 const* to(T2 const* ptr) { return dynamic_cast<T1 const*>(ptr); }

template<typename T, typename... Args>
constexpr bool isIn(T v, Args... args) {
	return (... || (v == args));
}

constexpr unsigned int str2int(const char* str, int h = 0) {
	return !str[h] ? 5381 : (str2int(str, h+1) * 33) ^ str[h];
}

template <typename T, typename TT>
T get_from_map(const string_map<T>& map, string key, TT defValue) {
	if (map.count(key) > 0)
		return map.at(key);
	return defValue;
}

bool ends_with(const string& str, const string& suffix);

class TVMStack {
	int m_size;
	// map paramters or local variables to their absolute stack position
	string_map<int> m_params;

public:

	TVMStack() : m_size(0) {}

	int size() const {
		return m_size;
	}

	void change(int diff) {
		m_size += diff;
	}

	bool isParam(const string& name) const {
		return m_params.count(name) > 0;
	}

	void add(const string& name, bool doAllocation) {
		m_params[name] = doAllocation? m_size++ : m_size - 1;
	}

	std::vector<string> dropLocals(int savedStackSize) {
		std::vector<string> locals;
		for (const auto& p : m_params) {
			if (p.second >= savedStackSize)
				locals.push_back(p.first);
		}
		for (const auto& name : locals) {
			m_params.erase(name);
		}
		return locals;
	}

	int getOffset(const string& name) const {
		solAssert(isParam(name), "");
		return getOffset(m_params.at(name));
	}

	int getOffset(int stackPos) const {
		return m_size - 1 - stackPos;
	}

	std::string dumpParams() const {
		std::ostringstream o;
		for (const auto& p : m_params) {
			o << p.first << "=" << p.second << ", ";
		}
		return o.str();
	}
	
	void ensureSize(int savedStackSize, const string& location) const {
		solAssert(savedStackSize == m_size, "stack: " + toString(savedStackSize)
		                                    + " vs " + toString(m_size) + " at " + location);
	}

};

struct CodeLines {
	vector<string> lines;
	
	string str(const string& indent) const {
		std::ostringstream o;
		for (const string& s : lines) {
			o << indent << s << endl;
		}
		return o.str();
	}
	
	void push(const string& cmd) {
		if (cmd != "") {
			// space means empty line
			if (cmd == " ")
				lines.emplace_back("");
			else
				lines.push_back(cmd);
		}
	}
	
	void pushCont(const CodeLines& cont, const string& comment = "") {
		if (comment.empty())
			push("PUSHCONT {");
		else
			push("PUSHCONT { ; " + comment);
		for (const auto& l : cont.lines)
			push(string("\t") + l);
		push("}");
	}
	
	void append(const CodeLines& oth) {
		for (const auto& s : oth.lines) {
			lines.push_back(s);
		}
	}

	void generateGlobl(const string& fname, const bool isPublic) {
		push(".globl\t" + fname);
		if (isPublic) {
			push(".public\t" + fname);
		}
		push(".type\t"  + fname + ", @function");
	}

	void generateInternal(const string& fname, const int id) {
		push(".internal-alias :" + fname + ",        " + toString(id));
		push(".internal\t:" + fname);
	}

	void generateMacro(const string& functionName) {
		push(".macro " + functionName);
	}
};

struct ContInfo {
	bool canReturn = false;
	bool canBreak = false;
	bool canContinue = false;
	bool alwaysReturns = false;
	ContInfo() = default;
};

class TVMScanner: public ASTConstVisitor
{
	int m_loopDepth = 0;
public:
	explicit TVMScanner(const ASTNode& node) {
		node.accept(*this);
		solAssert(m_loopDepth == 0, "");
	}

protected:
	bool visit(WhileStatement const&) override {
		m_loopDepth++;
		return true;
	}

	void endVisit(WhileStatement const&) override {
		m_loopDepth--;
	}

	bool visit(ForStatement const&) override {
		m_loopDepth++;
		return true;
	}

	void endVisit(ForStatement const&) override {
		m_loopDepth--;
	}

	void endVisit(Return const&) override {
		m_info.canReturn = true;
	}

	void endVisit(Break const&) override {
		if (m_loopDepth == 0)
			m_info.canBreak = true;
	}

	void endVisit(Continue const&) override {
		if (m_loopDepth == 0)
			m_info.canContinue = true;
	}

public:
	ContInfo m_info;
};

template <typename T>
static bool doesAlways(const Statement* st) {
	auto rec = [] (const Statement* s) {
		return doesAlways<T>(s);
	};
	if (to<T>(st))
		return true;
	if (to<ExpressionStatement>(st) ||
		to<VariableDeclarationStatement>(st) ||
		to<EmitStatement>(st) ||
		to<PlaceholderStatement>(st) ||
		to<Assignment>(st))
		return false;
	if (to<Continue>(st) || to<Break>(st) || to<Return>(st))
		return false;
	if (auto block = to<Block>(st)) {
		for (const auto& s : block->statements()) {
			if (rec(s.get()))
				return true;
		}
		return false;
	}
	if (auto ifStatement = to<IfStatement>(st)) {
		if (!ifStatement->falseStatement())
			return false;
		return rec(&ifStatement->trueStatement()) && rec(ifStatement->falseStatement());
	}
	if (auto forStatement = to<ForStatement>(st)) {
		return rec(&forStatement->body());
	}
	if (auto whileStatement = to<WhileStatement>(st)) {
		return rec(&whileStatement->body());
	}
	solAssert(false, string("Unsupported statement type: ") + typeid(*st).name());
	return false;
}

ContInfo getInfo(const Statement& statement) {
	TVMScanner scanner(statement);
	ContInfo info = scanner.m_info;
	info.alwaysReturns = doesAlways<Return>(&statement);
	return info;
}

bool isAddressType(const Type* type) {
	return to<AddressType>(type) || to<ContractType>(type);
}

bool isTvmCell(const Type* type) {
	auto structType = to<StructType>(type);
	return structType && structType->structDefinition().name() == "TvmCell";
}

struct AddressInfo {
	static int minBitLength() {
		// addr_var$11 anycast:(Maybe Anycast) addr_len:(## 9) workchain_id:int32 address:(bits addr_len) = MsgAddressInt;
		return 2 + 1 + 9 + 32 + 1;
	}

	static int maxBitLength() {
		// addr_std$10 anycast:(Maybe Anycast) workchain_id:int8 address:bits256 = MsgAddressInt
		// anycast_info$_ depth:(#<= 30) { depth >= 1 } rewrite_pfx:(bits depth) = Anycast;
		return 2 + 1 + (2 * 30) + 8 + 256;
	}
};

int bitsForEnum(size_t val_count) {
	int bytes = 0;
	val_count--;
	while (true) {
		val_count >>= 8;
		++bytes;
		if (val_count == 0) {
			break;
		}
	}
	return 8 * bytes;
}

struct TypeInfo {
	bool isNumeric {false};
	bool isSigned {false};
	int numBits {0};
	Type::Category category {Type::Category::Integer};

	explicit TypeInfo(const Type* type) {
		if (auto* integerType = to<IntegerType>(type)) {
			isNumeric = true;
			isSigned = integerType->isSigned();
			numBits = integerType->numBits();
		} else if (to<BoolType>(type)) {
			isNumeric = true;
			isSigned = true;
			numBits = 1;
		} else if (auto* fixedBytesType = to<FixedBytesType>(type)) {
			isNumeric = true;
			isSigned = false;
			numBits = 8 * static_cast<int>(fixedBytesType->numBytes());
		} else if (auto* enumType = to<EnumType>(type)) {
			isNumeric = true;
			isSigned = false;
			numBits = bitsForEnum(enumType->numberOfMembers());
		}
	}
};

bool isTvmIntrinsic(const string& name) {
	return 0 == name.find("tvm_");
}

const Type* getType(const Expression* expr) {
	return expr->annotation().type.get();
}

const Type* getType(const VariableDeclaration* var) {
	return var->annotation().type.get();
}

bool isIntegralType(const Type* type) {
	return TypeInfo(type).isNumeric;
}

bool isStringOrStringLiteralOrBytes(const Type* type) {
	auto arrayType = to<ArrayType>(type);
	return type->category() == Type::Category::StringLiteral || (arrayType && arrayType->isByteArray());
}

std::string getKeyDict(Type const* keyType) {
	TypeInfo ti(keyType);
	if (ti.isNumeric) {
		return ti.isSigned? "I" : "U";
	} else if (isStringOrStringLiteralOrBytes(keyType)) {
		return "U";
	}
	return ""; // dict key is slice
}

int getKeyDictLength(Type const* key) {
	TypeInfo ti{key};
	if (ti.isNumeric) {
		return ti.numBits;
	}
	if (isStringOrStringLiteralOrBytes(key)){
		return 256;
	}
	solAssert(false, "");
}

IntegerType getKeyTypeOfC4() {
	return IntegerType(TvmConst::C4::KeyLength);
}

IntegerType getKeyTypeOfArray() {
	return IntegerType(TvmConst::ArrayKeyLength);
}

string storeIntegralOrAddress(const Type* type, bool reverse) {
	if (isAddressType(type))
		return reverse ? "STSLICER" : "STSLICE";
	auto ti = TypeInfo(type);
	if (ti.isNumeric) {
		string cmd = ti.isSigned? "STI" : "STU";
		if (reverse) cmd = cmd + "R";
		solAssert(cmd != "STU 267", "");
		return cmd + " " + toString(ti.numBits);
	}
	solAssert(false, "Unsupported param type " + type->toString());
}

string loadIntegralOrAddress(const Type* type) {
	if (isAddressType(type)) {
		return "LDMSGADDR";
	}
	auto ti = TypeInfo(type);
	solAssert(ti.isNumeric, "");
	string cmd = ti.isSigned? "LDI " : "LDU ";
	return cmd + toString(ti.numBits);
}

string preloadIntergalOrAddress(const Type* type) {
	return "P" + loadIntegralOrAddress(type);
}

bool isExpressionExactTypeKnown(Expression const* expr) {
	if (to<Literal>(expr)) return true;
	if (to<Identifier>(expr)) return true;
	if (to<FunctionCall>(expr)) return true;
	if (to<IndexAccess>(expr)) return true;
	if (to<MemberAccess>(expr)) return true;
	return false;
}

bool isNonNegative(Expression const* expr) {
	auto type = getType(expr);
	if (isExpressionExactTypeKnown(expr)) {
		if (auto type2 = to<RationalNumberType>(type)) {
			if (!type2->integerType()->isSigned())
				return true;
		}
		if (auto type2 = to<IntegerType>(type)) {
			if (!type2->isSigned())
				return true;
		}
	}
	if (auto binaryOp = to<BinaryOperation>(expr)) {
		if (isNonNegative(&binaryOp->leftExpression()) && isNonNegative(&binaryOp->rightExpression())) {
			switch (binaryOp->getOperator()) {
				case Token::Add:
				case Token::Mul:
					return true;
				default:
					break;
			}
		}
	}
	return false;
}

vector<ContractDefinition const*> getContractsChain(ContractDefinition const* contract) {
	vector<FunctionDefinition const*> result;
	auto contracts = contract->annotation().linearizedBaseContracts;
	std::reverse(contracts.begin(), contracts.end());
	return contracts;
}

auto getContractFunctionPairs(ContractDefinition const* contract) {
	vector<pair<FunctionDefinition const*, ContractDefinition const*>> result;
	for (ContractDefinition const* c : getContractsChain(contract)) {
		for (const auto f : c->definedFunctions())
			result.push_back(make_pair(f, c));
	}
	return result;
}

auto getFunction(ContractDefinition const* contract, const string& functionName) {
	const FunctionDefinition* result = nullptr;
	for (const auto f : contract->definedFunctions()) {
		if (f->name() == functionName)
			return f;
	}
	return result;
}

bool isSuper(Expression const* expr) {
	if (auto identifier = to<Identifier>(expr)) {
		return identifier->name() == "super";
	}
	return false;
}

bool isMacro(const std::string& functionName) {
	return ends_with(functionName, "_macro");
}

bool isAddressThis(const FunctionCall* fcall) {
	if (!fcall)
		return false;
	auto arguments = fcall->arguments();
	if (auto etn = to<ElementaryTypeNameExpression>(&fcall->expression())) {
		if (etn->typeName().token() == Token::Address) {
			solAssert(arguments.size() == 1, "");
			if (auto arg0 = to<Identifier>(arguments[0].get())) {
				if (arg0->name() == "this")
					return true;
			}
		}
	}
	return false;
}

// List of all function with a given name
auto getContractFunctions(ContractDefinition const* contract, const string& fname) {
	vector<FunctionDefinition const*> result;
	for (auto pair : getContractFunctionPairs(contract)) {
		if (pair.first->name() == fname)
			result.push_back(pair.first);
	}
	return result;
}

// List of all contract functions including derived
auto getContractFunctions(ContractDefinition const* contract) {
	vector<FunctionDefinition const*> result;
	for (auto pair : getContractFunctionPairs(contract)) {
		auto f = pair.first;
		auto fn = f->name();
		if (isTvmIntrinsic(fn))
			continue;
		// TODO: not needed check?
		if (!f->isConstructor() && f != getContractFunctions(contract, fn).back())
			continue;
		result.push_back(f);
	}
	return result;
}

const ContractDefinition* getSuperContract(const ContractDefinition* currentContract,
														 const ContractDefinition* mainContract,
														 string fname) {
	ContractDefinition const* prev = nullptr;
	for (auto c : getContractsChain(mainContract)) {
		if (c == currentContract)
			break;
		if (getFunction(c, fname))
			prev = c;
	}
	return prev;
}

bool ends_with(const string& str, const string& suffix) {
	if (suffix.size() > str.size())
		return false;
	return 0 == str.compare(str.size()-suffix.size(), suffix.size(), suffix);
}

std::string ASTNode2String(const ASTNode& node, const string& error_messag = "", bool isWarning = false) {
	ErrorList		m_errors;
	ErrorReporter	m_errorReporter(m_errors);
	m_errorReporter.parserError(node.location(), error_messag);
	string message = SourceReferenceFormatter::formatExceptionInformation(
		*(m_errorReporter.errors())[0],
		isWarning ? "Warning" : "Error"
	);
	return message;
}

[[noreturn]]
void cast_error(const ASTNode& node, const string& error_message) {
	cerr << ASTNode2String(node, error_message) << endl;
	std::exit(EXIT_FAILURE);
}

void cast_warning(const ASTNode& node, const string& error_message) {
	cerr << ASTNode2String(node, error_message, true) << endl;
}

struct FuncInfo {
	const FunctionDefinition*	m_function;
	const ContractDefinition*	m_contract;
	string						m_internalName;

	FuncInfo(const FunctionDefinition* f, const ContractDefinition* c)
		: m_function(f), m_contract(c) 
	{
	}
};

class TVMCompilerContext {
	const ContractDefinition*				m_contract = nullptr;
	string_map<const FunctionDefinition*>	m_functions;
	map<const FunctionDefinition*, const ContractDefinition*>	m_function2contract;
	string_map<int>							m_members;
	string_map<const EventDefinition*>		m_events;
	set<string>								m_allContractNames;
	bool haveFallback = false;
	bool haveOnBounce = false;

	void addMember(VariableDeclaration const* variable) {
		if (variable->isConstant()){
			return;
		}
		const string& name = variable->name();
		solAssert(m_members.count(name) == 0, "Duplicate member variable " + name);
		m_members[name] = m_members.size() + TvmConst::C4::PersistenceMembersStartIndex;
	}

	void addEvent(EventDefinition const *event) {
		std::string name = event->name();
		solAssert(m_events.count(name) == 0, "Duplicate event " + name);
		m_events[name] = event;
	}

	void addFunction(FunctionDefinition const* _function) {
		if (!_function->isConstructor()) {
			string name = _function->name();
			if (m_functions.count(name) > 0) {
				// TODO: check for signature match!
				cast_error(*_function, "Function overloading is not supported yet");
			}
			m_functions[name] = _function;
		}
	}
	
	void initMembers(ContractDefinition const* contract, const std::vector<ContractDefinition const*>& allContracts) {
		solAssert(!m_contract, "");
		m_contract = contract;
		for (ContractDefinition const* c : getContractsChain(contract)) {
			for (VariableDeclaration const* variable: c->stateVariables())
				addMember(variable);
		}
		for (ContractDefinition const* c : getContractsChain(contract)) {
			for (EventDefinition const *event : c->events())
				addEvent(event);
		}
		for (const auto pair : getContractFunctionPairs(contract)) {
			m_function2contract.insert(pair);
		}
		map<ASTString, vector<TypePointer>> FuncToParams;
		for (auto c: getContractsChain(contract)) {
			for (auto f : getContractFunctions(c)) {
				if (f->isConstructor())
					continue;
				vector<TypePointer> params;
				for (const auto& param: f->parameters())
					params.push_back(param->annotation().type);
				auto presentParams = FuncToParams.find(f->name());
				if (presentParams != FuncToParams.end()) {
					if (presentParams->second.size() != params.size())
						cast_error(*f, "Overloading of methods is not allowed.");
					for (size_t i = 0; i < params.size(); i++) {
						if (*params[i] != *presentParams->second[i])
							cast_error(*f, "Overloading of methods is not allowed.");
					}
				} else {
					FuncToParams.insert({f->name(), params});
				}
			}
		}
		for (const auto f : getContractFunctions(contract)) {
			haveFallback |= f->isFallback();
			haveOnBounce |= f->name() == "onBounce";
			if (isPureFunction(f))
				continue;
			addFunction(f);
		}
		for (const auto pair : getContractFunctionPairs(contract)) {
			auto f = pair.first;
			auto c = pair.second;
			if (!isTvmIntrinsic(f->name()) && !isPureFunction(f)) {
				FuncInfo fi(f, c);
				fi.m_internalName = getFunctionInternalName(f);
				if (!f->isConstructor() && f != getContractFunctions(contract, f->name()).back()) {
					fi.m_internalName = c->name() + "_" + f->name();
				}
				m_functionsList.push_back(fi);
			}
		}
		for (auto c : allContracts) {
			m_allContractNames.insert(c->name());
		}
	}
	
public:
	TVMCompilerContext(ContractDefinition const* contract, const std::vector<ContractDefinition const*>& allContracts) {
		initMembers(contract, allContracts);
	}
	
	mutable set<string>		m_remoteFunctions;
	vector<FuncInfo>		m_functionsList;
	const FuncInfo*			m_currentFunction = nullptr;
	map<string, CodeLines>	m_inlinedFunctions;

	bool isStdlib() const {
		return m_contract->name() == "stdlib";
	}

	string getFunctionInternalName(FunctionDefinition const* _function) const {
		if (isStdlib())
			return _function->name();
		if (_function->isConstructor()) {
			auto contract = getContract(_function);
			solAssert(contract, "");
			return "constructor_" + contract->name();
		}
        if (_function->isFallback()) {
            return "fallback_internal";
        }
		return _function->name() + "_internal";
	}

	static string getFunctionExternalName(FunctionDefinition const* _function) {
		const string& fname = _function->name();
		solAssert(_function->isPublic(), "Internal error: expected public function: " + fname);
		if (_function->isConstructor()) {
			return "constructor";
		}
		if (_function->isFallback()) {
			return "fallback";
		}
		return getFunctionExternalName(fname);
	}
	
	static string getFunctionExternalName(const string& fname) {
		return fname;
	}

	bool isPureFunction(FunctionDefinition const* f) const {
		const auto& vec = getContract(f)->annotation().unimplementedFunctions;
		return std::find(vec.cbegin(), vec.cend(), f) != vec.end();
	}
	
	const ContractDefinition* getContract() const {
		return m_contract;
	}
	
	const ContractDefinition* getContract(const FunctionDefinition* f) const {
		return m_function2contract.at(f);
	}

	int getMemberIdx(const string& name) const {
		return get_from_map(m_members, name, -1);
	}
	
	const FunctionDefinition* getLocalFunction(string fname) const {
		return get_from_map(m_functions, std::move(fname), nullptr);
	}
	
	const EventDefinition* getEvent(string name) const {
		return get_from_map(m_events, name, nullptr);
	}
	
	bool isContractName(const string& name) const {
		return m_allContractNames.count(name) > 0;
	}

	bool haveFallbackFunction() const {
		return haveFallback;
	}

	bool haveOnBounceHandler() const {
		return haveOnBounce;
	}

	std::vector<const EventDefinition*> events() const {
		std::vector<const EventDefinition*> result;
		for (const auto& [name, event] : m_events) {
			(void)name;
			result.push_back(event);
		}
		return result;
	}
};

class IStackPusher {
public:
	virtual void push(int stackDiff, const string& cmd) = 0;
	virtual TVMStack& getStack() = 0;
};

struct StackPusherImpl : IStackPusher {
	TVMStack&					m_stack;
	CodeLines&					m_code;
	
	StackPusherImpl(TVMStack& stack, CodeLines&	code) 
		: m_stack(stack)
		, m_code(code) 
		{}
	
	void push(int stackDiff, const string& cmd) override {
		m_code.push(cmd);
		m_stack.change(stackDiff);
	}
	
	TVMStack& getStack() override {
		return m_stack;
	}
};

struct StackPusherImpl2 : IStackPusher {
	TVMStack m_stack;
	CodeLines m_code;

	explicit StackPusherImpl2() {

	};

	void push(int stackDiff, const string& cmd) override {
		m_code.push(cmd);
		m_stack.change(stackDiff);
	}

	CodeLines& codeLines() {
		return m_code;
	}

	TVMStack& getStack() override {
		return m_stack;
	}
};

struct ABITypeSize {
	int minBits = -1;
	int maxBits = -1;
	int refs = -1;

	explicit ABITypeSize(Type const* type) {
		if (isAddressType(type)){
			minBits = AddressInfo::minBitLength();
			maxBits = AddressInfo::maxBitLength();
			refs = 0;
		} else if (isIntegralType(type)) {
			TypeInfo ti{type};
			solAssert(ti.isNumeric, "");
			minBits = ti.numBits;
			maxBits = ti.numBits;
			refs = 0;
		} else if (auto arrayType = to<ArrayType>(type)) {
			if (arrayType->isByteArray()) {
				minBits = 0;
				maxBits = 0;
				refs = 1;
			} else {
				minBits = 32;
				maxBits = 32;
				refs = 1;
			}
		} else if (isTvmCell(type)) {
			minBits = 0;
			maxBits = 0;
			refs = 1;
		} else {
			solAssert(false, "");
		}
	}
};

class StackPusherHelper {
private:
	IStackPusher* const					m_pusher;
	const TVMCompilerContext* const		m_ctx;

public:
	StackPusherHelper(IStackPusher* compiler, const TVMCompilerContext* ctx) 
		: m_pusher(compiler), m_ctx(ctx) {}
		
	const TVMCompilerContext&  ctx() const {
		return *m_ctx;
	}

	TVMStack& getStack() {
		return m_pusher->getStack();
	}
	
	auto getStackPusher() const {
		return m_pusher;
	}

	void pushLog(const std::string& str) {
		push(0, "PRINTSTR " + str);
	}

	void pushLines(const std::string& lines) {
		std::istringstream stream{lines};
		std::string line;
		while (std::getline(stream, line)) {
			push(0, line);
		}
	}

	void push(int stackDiff, const string& cmd) {
		solAssert(m_pusher, "#18");
		m_pusher->push(stackDiff, cmd);
	}

	void pushInt(int i) {
		push(+1, "PUSHINT " + toString(i));
	}

	void pushZeroAddress() {
		push(+1, "PUSHSLICE x8000000000000000000000000000000000000000000000000000000000000000001_");
	}

	void literalToSliceAddress(Literal const* literal) {

		Type const* type = literal->annotation().type.get();
		dev::u256 value = type->literalValue(literal);


//		addr_std$10 anycast:(Maybe Anycast) workchain_id:int8 address:bits256 = MsgAddressInt;

		std::string s;
		s += "10";
		s += "0";
		s += std::string(8, '0');
		for (int i = 0; i < 256; ++i) {
			s += value % 2 == 0? "0" : "1";
			value /= 2;
		}
		std::reverse(s.rbegin(), s.rbegin() + 256);
		bool haveCompletionTag = false;
		if (s.size() % 4 != 0) {
			haveCompletionTag = true;
			s += "1";
			s += std::string((4 - s.size() % 4) % 4, '0');
		}

		std::string ans;
		for (int i = 0; i < static_cast<int>(s.length()); i += 4) {
			int x = stoi(s.substr(i, 4), nullptr, 2);
			std::stringstream sstream;
			sstream << std::hex << x;
			ans += sstream.str();
		}
		if (haveCompletionTag) {
			ans += "_";
		}
		push(+1, "PUSHSLICE x" + ans);
	}

	bool tryImplicitConvert(Type const *leftType, Type const *rightType) {
		if (leftType->category() == Type::Category::FixedBytes && rightType->category() == Type::Category::StringLiteral) {
			auto stringLiteralType = to<StringLiteralType>(rightType);
			dev::u256 value = 0;
			for (char c : stringLiteralType->value()) {
				value = value * 256 + c;
			}
			push(+1, "PUSHINT " + toString(value));
			return true;
		}
		return false;
	}

	void push(const CodeLines& codeLines) {
		for (const std::string& s : codeLines.lines) {
			push(0, s);
		}
	}
	
	void pushPrivateFunctionOrMacroCall(const int stackDelta, const string& fname) {
		push(stackDelta, "CALL $" + fname + "$");
	}

	void pushCall(const string& functionName, const FunctionType* ft) {
		int params  = ft->parameterTypes().size();
		int retVals = ft->returnParameterTypes().size();
		push(-params + retVals, "CALL $" + functionName + "$");
	}

	void pushFunctionIndex(const string& fname) {
		push(+1, "PUSHINT $" + fname + "$");
	}

	void dumpStackSize(const string& prefix = "") {
		push(0, prefix + ";; stack=" + toString(m_pusher->getStack().size()));
	}
	
	void drop(int cnt) {
		solAssert(cnt >= 0, "");
		if (cnt == 0)
			return;

		if (cnt == 1) {
			push(-1, "DROP");
		}
		else {
			if (cnt > 15) {
				pushInt(cnt);
				push(-(cnt + 1), "DROPX");
			} else {
				push(-cnt, "BLKDROP " + toString(cnt));
			}
		}
	}

	void blockSwap(int m, int n) {
		if (m == 0 || n == 0) {
			return;
		}
		push(0, "BLKSWAP " + toString(m) + ", " + toString(n));
	}

	void dropUnder(int leftCount, int droppedCount) {
		// drop dropCount elements that are situated under top leftCount elements
		solAssert(leftCount >= 0, "");
		solAssert(droppedCount >= 0, "");

		auto f = [this, leftCount, droppedCount](){
			if (droppedCount > 15 || leftCount > 15) {
				pushInt(droppedCount);
				pushInt(leftCount);
				push(-2, "BLKSWX");
			} else {
				push(0, "BLKSWAP " + toString(droppedCount) + ", " + toString(leftCount));
			}
			drop(droppedCount);
		};

		if (droppedCount == 0) {
			// nothing do
		} else if (leftCount == 0) {
			drop(droppedCount);
		} else if (droppedCount == 1) {
			if (leftCount == 1) {
				push(-1, "NIP");
			} else {
				f();
			}
		} else if (droppedCount == 2) {
			if (leftCount == 1) {
				push(-1, "NIP");
				push(-1, "NIP");
			} else {
				f();
			}
		} else {
			if (leftCount == 1) {
				push(0, "XCHG s" + toString(droppedCount));
				drop(droppedCount);
			} else {
				f();
			}
		}
	}

	void setRootItem(const string& name, const Type* dictValueType, ASTNode const& node) {
		// value
		prepareValueForDictOperations(dictValueType); // value'
		pushInt(ctx().getMemberIdx(name)); // value' key
		pushPersistentDataDict(); // value' key dict
		setDict(getKeyTypeOfC4(), *dictValueType, node); // dict

		push(+1, "PUSH c7");
		push(0, "SWAP");
		push(-1, "SETSECOND");
		push(-1, "POP c7");
	}

	void prepareKeyForDictOperations(Type const* key) {
	    // stack: key
	    if (isIn(key->category(), Type::Category::Address, Type::Category::Contract)) {
		    // addr dict
		    push(+1, "NEWC"); // addr dict builder
		    push(+1, "PUSH s2"); // addr dict builder addr
		    push(-1, "STSLICER"); // addr dict builder
		    pushInt(AddressInfo::maxBitLength()); // addr dict builder n maxL
		    push(+1, "PUSH s3"); // addr dict builder n addr
		    push(0, "SBITS"); // addr dict builder n
		    push(-1, "SUB"); // addr dict builder l
		    push(-1, "STZEROES"); // addr dict builder
		    push(0, "ENDC CTOS"); // addr dict builder
		    push(-1, "POP s2"); // addr dict
	    } else if (isStringOrStringLiteralOrBytes(key)) {
		    push(+1, "PUSH s1"); // str dict str
		    push(-1 + 1, "HASHCU"); // str dict hash
		    push(-1, "POP s2"); // hash dict
	    }
	}

	void prepareValueForDictOperations(Type const* dictValueType) {
		if (isIntegralType(dictValueType)) {
			push(0, "NEWC " + storeIntegralOrAddress(dictValueType, false));
		}
	}

	static TypePointer parseIndexType(Type const* type) {
		if (to<ArrayType>(type)) {
			return TypePointer(new IntegerType(32));
		}
		if (auto mappingType = to<MappingType>(type)) {
			return mappingType->keyType();
		}
		solAssert(false, "");
	}

	void setDict(Type const &keyType, Type const &valueType, ASTNode const& node) {
		// stack: value index dict
		if (isIn(keyType.category(), Type::Category::Address, Type::Category::Contract)) {
			pushInt(AddressInfo::maxBitLength()); // value index dict nBits
		} else if (isStringOrStringLiteralOrBytes(&keyType)) {
			pushInt(256);
		} else {
			TypeInfo ti{&keyType};
			solAssert(ti.isNumeric, "");
			pushInt(ti.numBits); // stack: value index dict nBits
		}

		// stack: value index dict keyBitLength
		string dict_cmd;
		switch (valueType.category()) {
			case Type::Category::Address:
			case Type::Category::Contract:
			case Type::Category::Struct:
				if (isTvmCell(&valueType)) {
					dict_cmd = "DICT" + getKeyDict(&keyType) + "SETGETOPTREF DROP";
				} else {
					dict_cmd = "DICT" + getKeyDict(&keyType) + "SET";
				}
				break;
			case Type::Category::Integer:
			case Type::Category::Bool:
			case Type::Category::FixedBytes:
			case Type::Category::Enum:
				dict_cmd = "DICT" + getKeyDict(&keyType) + "SETB";
				break;
			case Type::Category::Array:
			case Type::Category::Mapping:
				dict_cmd = "DICT" + getKeyDict(&keyType) + "SETGETOPTREF DROP";
				break;
			default:
				cast_error(node, "Unsupported value type: " + valueType.toString());
		}

		push(-3, dict_cmd);
	}

	void pushPersistentDataDict() {
		push(+1, "PUSH c7 SECOND");
	}

	bool tryAssignParam(const string& name) {
		auto& stack = m_pusher->getStack();
		if (stack.isParam(name)) {
			int idx = stack.getOffset(name);
			solAssert(idx > 0, "");
			if (idx == 1) {
				push(-1, "NIP");
			} else {
				push(-1, "POP s" + toString(idx));
			}
			return true;
		}
		return false;
	}
	
	void getFromDict(const Type& keyType, const Type& valueType, ASTNode const& node);

	void pushCont(const CodeLines& cont, const string& comment = "") {
		if (comment.empty())
			push(0, "PUSHCONT {");
		else
			push(0, "PUSHCONT { ; " + comment);
		for (const auto& l : cont.lines)
			push(0, string("\t") + l);
		push(+1, "}"); // adjust stack
	}
	
	void ensureValueFitsType(const ElementaryTypeNameToken& typeName, const ASTNode& node) {
		push(0, ";; " + typeName.toString());
		switch (typeName.token()) {
			case Token::IntM:
				push(0, "FITS " + toString(typeName.firstNumber()));
				break;
			case Token::UIntM:
				push(0, "UFITS " + toString(typeName.firstNumber()));
				break;
			case Token::BytesM:
				push(0, "UFITS " + toString(8 * typeName.firstNumber()));
				break;
			case Token::Int:
				push(0, "FITS 256");
				break;
			case Token::Address:
				// Address is a slice
				break;
			case Token::UInt:
				push(0, "UFITS 256");
				break;
			case Token::Bool:
				push(0, "FITS 1");
				break;
			default:
				cast_error(node, "Unimplemented casting");
		}
	}

	enum class ReasonOfOutboundMessage {
		EmitEventExternal,
		FunctionReturnExternal,
		RemoteCallInternal
	};

	class EncodePosition : private boost::noncopyable {
		int restSliceBits;
		int restFef;
		int qtyOfCreatedBuilders;

	public:
		explicit EncodePosition(int bits) :
				restSliceBits{TvmConst::CellBitLength - bits},
				restFef{3},
				qtyOfCreatedBuilders{0}
		{

		}

		bool needNewCell(Type const* type) {
			ABITypeSize size(type);
			solAssert(0 <= size.refs && size.refs <= 1, "");

			restSliceBits -= size.maxBits;
			restFef -= size.refs;

			if (restSliceBits < 0 || restFef == 0) {
				restSliceBits =  TvmConst::CellBitLength - size.maxBits;
				restFef = 4 - size.refs;
				++qtyOfCreatedBuilders;
				return true;
			}
			return false;
		}

		int countOfCreatedBuilders() {
			return qtyOfCreatedBuilders;
		}
	};

	void encodeFunctionAndParams(const string& functionName,
	                             const std::vector<Type const*>& types,
	                             const std::vector<ASTNode const*>& nodes,
	                             const std::function<void(size_t)>& pushParam, const ReasonOfOutboundMessage& reason) {
		push(+1, "NEWC");
		push(+1, "PUSHINT $" + functionName + "$");
		switch (reason) {
			case ReasonOfOutboundMessage::FunctionReturnExternal:
				push(+1, "PUSHINT " + to_string(0x80000000));
				push(-1, "OR");
				break;

			case ReasonOfOutboundMessage::EmitEventExternal:
				push(+1, "PUSHINT " + to_string(0x7fffffff));
				push(-1, "AND");
				break;

			default:
				break;
		}

		push(-1, "STUR 32");
		EncodePosition position{32};

		encodeParameters(types, nodes, pushParam, position);
	}

	void encodeParameters(const std::vector<Type const*>& types,
	                      const std::vector<ASTNode const*>& nodes,
	                      const std::function<void(size_t)>& pushParam,
	                      EncodePosition& position) {
		// builder must be situated on top stack
		solAssert(types.size() == nodes.size(), "");
		for (size_t idx = 0; idx < types.size(); idx++) {
			auto type = types[idx];
			encodeParameter(type, position, [&](){pushParam(idx);}, nodes[idx]);
		}
		for (int idx = 0; idx < position.countOfCreatedBuilders(); idx++) {
			push(-1, "STBREFR");
		}
	}

	void encodeParameter(Type const* type, EncodePosition& position, const std::function<void()>& pushParam, ASTNode const* node) {
		// stack: builder...
		if (auto structType = to<StructType>(type); structType && !isTvmCell(structType)) {
			pushParam(); // builder... struct
			encodeStruct(structType, node, position); // stack: builder...
		} else {
			if (position.needNewCell(type)) {
				push(+1, "NEWC");
			}

			if (isIntegralType(type) || isAddressType(type)) {
				pushParam();
				push(-1, storeIntegralOrAddress(type, true));
			} else if (auto arrayType = to<ArrayType>(type)) {
				if (arrayType->isByteArray()) {
					pushParam();
					push(-1, "STREFR");
				} else {
					pushParam();
					pushPrivateFunctionOrMacroCall(-2 + 1, "encode_array_macro");
				}
			} else if (isTvmCell(structType)) {
				pushParam();
				push(-1, "STREFR");
			} else {
				cast_error(*node, "Unsupported type : " + type->toString());
			}
		}
	}

	void f(const StructDefinition &structDefinition, const string &pref, const std::map<std::string, int> &memberToStackSize,
			EncodePosition& position, ASTNode const* node);
	void encodeStruct(const StructType* structType, ASTNode const* node, EncodePosition& position);
	void pushDefaultValue(Type const* type);
};

class ITVMCompiler : public IStackPusher {
public:
	virtual const FunctionDefinition* getRemoteFunctionDefinition(const MemberAccess* memberAccess)	= 0;
	virtual CodeLines proceedContinuationExpr(const Expression& expression)		= 0;
	virtual void applyContinuation(const CodeLines& lines) 						= 0;
};

class IExpressionCompiler {
public:
	virtual void acceptExpr(const Expression* expr) = 0;
	virtual void acceptExpr2(const Expression* expr, const bool isResultNeeded = true) = 0;
};

}	// solidity
}	// dev