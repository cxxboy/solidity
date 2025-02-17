/*
	This file is part of solidity.
	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.
	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <test/libsolidity/SemanticTest.h>

#include <libsolutil/Whiskers.h>
#include <libyul/Exceptions.h>
#include <test/Common.h>
#include <test/libsolidity/util/BytesUtils.h>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/throw_exception.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

using namespace std;
using namespace solidity;
using namespace solidity::yul;
using namespace solidity::langutil;
using namespace solidity::util;
using namespace solidity::util::formatting;
using namespace solidity::frontend::test;
using namespace boost;
using namespace boost::algorithm;
using namespace boost::unit_test;
namespace fs = boost::filesystem;


SemanticTest::SemanticTest(
	string const& _filename,
	langutil::EVMVersion _evmVersion,
	vector<boost::filesystem::path> const& _vmPaths,
	bool _enforceViaYul,
	bool _enforceGasCost,
	u256 _enforceGasCostMinValue
):
	SolidityExecutionFramework(_evmVersion, _vmPaths),
	EVMVersionRestrictedTestCase(_filename),
	m_sources(m_reader.sources()),
	m_lineOffset(m_reader.lineNumber()),
	m_enforceViaYul(_enforceViaYul),
	m_enforceGasCost(_enforceGasCost),
	m_enforceGasCostMinValue(_enforceGasCostMinValue)
{
	initializeBuiltins();

	string choice = m_reader.stringSetting("compileViaYul", "default");
	if (choice == "also")
	{
		m_runWithYul = true;
		m_runWithoutYul = true;
	}
	else if (choice == "true")
	{
		m_runWithYul = true;
		m_runWithoutYul = false;
	}
	else if (choice == "false")
	{
		m_runWithYul = false;
		m_runWithoutYul = true;
		// Do not try to run via yul if explicitly denied.
		m_enforceViaYul = false;
	}
	else if (choice == "default")
	{
		m_runWithYul = false;
		m_runWithoutYul = true;
	}
	else
		BOOST_THROW_EXCEPTION(runtime_error("Invalid compileViaYul value: " + choice + "."));

	string compileToEwasm = m_reader.stringSetting("compileToEwasm", "false");
	if (compileToEwasm == "also")
		m_runWithEwasm = true;
	else if (compileToEwasm == "false")
		m_runWithEwasm = false;
	else
		BOOST_THROW_EXCEPTION(runtime_error("Invalid compileToEwasm value: " + compileToEwasm + "."));

	if (m_runWithEwasm && !m_runWithYul)
		BOOST_THROW_EXCEPTION(runtime_error("Invalid compileToEwasm value: " + compileToEwasm + ", compileViaYul need to be enabled."));

	// run ewasm tests only, if an ewasm evmc vm was defined
	if (m_runWithEwasm && !m_supportsEwasm)
		m_runWithEwasm = false;

	m_runWithABIEncoderV1Only = m_reader.boolSetting("ABIEncoderV1Only", false);
	if (m_runWithABIEncoderV1Only && !solidity::test::CommonOptions::get().useABIEncoderV1)
		m_shouldRun = false;

	auto revertStrings = revertStringsFromString(m_reader.stringSetting("revertStrings", "default"));
	soltestAssert(revertStrings, "Invalid revertStrings setting.");
	m_revertStrings = revertStrings.value();

	m_allowNonExistingFunctions = m_reader.boolSetting("allowNonExistingFunctions", false);

	parseExpectations(m_reader.stream());
	soltestAssert(!m_tests.empty(), "No tests specified in " + _filename);

	if (m_enforceGasCost)
	{
		m_compiler.setMetadataFormat(CompilerStack::MetadataFormat::NoMetadata);
		m_compiler.setMetadataHash(CompilerStack::MetadataHash::None);
	}
}

void SemanticTest::initializeBuiltins()
{
	solAssert(m_builtins.count("smokeTest") == 0, "");
	m_builtins["smokeTest"] = [](FunctionCall const&) -> std::optional<bytes>
	{
		return util::toBigEndian(u256(0x1234));
	};
}

TestCase::TestResult SemanticTest::run(ostream& _stream, string const& _linePrefix, bool _formatted)
{
	TestResult result = TestResult::Success;
	bool compileViaYul = m_runWithYul || m_enforceViaYul;

	if (m_runWithoutYul)
		result = runTest(_stream, _linePrefix, _formatted, false, false);

	if (compileViaYul && result == TestResult::Success)
		result = runTest(_stream, _linePrefix, _formatted, true, false);

	if (m_runWithEwasm && result == TestResult::Success)
		result = runTest(_stream, _linePrefix, _formatted, true, true);

	return result;
}

TestCase::TestResult SemanticTest::runTest(
	ostream& _stream,
	string const& _linePrefix,
	bool _formatted,
	bool _compileViaYul,
	bool _compileToEwasm
)
{
	bool success = true;
	m_gasCostFailure = false;

	if (_compileViaYul && _compileToEwasm)
		selectVM(evmc_capabilities::EVMC_CAPABILITY_EWASM);
	else
		selectVM(evmc_capabilities::EVMC_CAPABILITY_EVM1);

	reset();

	m_compileViaYul = _compileViaYul;
	if (_compileToEwasm)
	{
		soltestAssert(m_compileViaYul, "");
		m_compileToEwasm = _compileToEwasm;
	}

	m_compileViaYulCanBeSet = false;

	if (_compileViaYul)
		AnsiColorized(_stream, _formatted, {BOLD, CYAN}) << _linePrefix << "Running via Yul:" << endl;

	for (TestFunctionCall& test: m_tests)
		test.reset();

	map<string, solidity::test::Address> libraries;

	bool constructed = false;

	for (TestFunctionCall& test: m_tests)
	{
		if (constructed)
		{
			soltestAssert(
				test.call().kind != FunctionCall::Kind::Library,
				"Libraries have to be deployed before any other call."
			);
			soltestAssert(
				test.call().kind != FunctionCall::Kind::Constructor,
				"Constructor has to be the first function call expect for library deployments."
			);
		}
		else if (test.call().kind == FunctionCall::Kind::Library)
		{
			soltestAssert(
				deploy(test.call().signature, 0, {}, libraries) && m_transactionSuccessful,
				"Failed to deploy library " + test.call().signature);
			libraries[test.call().signature] = m_contractAddress;
			continue;
		}
		else
		{
			if (test.call().kind == FunctionCall::Kind::Constructor)
				deploy("", test.call().value.value, test.call().arguments.rawBytes(), libraries);
			else
				soltestAssert(deploy("", 0, bytes(), libraries), "Failed to deploy contract.");
			constructed = true;
		}

		if (test.call().kind == FunctionCall::Kind::Storage)
		{
			test.setFailure(false);
			bytes result(1, !storageEmpty(m_contractAddress));
			test.setRawBytes(result);
			soltestAssert(test.call().expectations.rawBytes().size() == 1, "");
			if (test.call().expectations.rawBytes() != result)
				success = false;
		}
		else if (test.call().kind == FunctionCall::Kind::Constructor)
		{
			if (m_transactionSuccessful == test.call().expectations.failure)
				success = false;
			if (success && !checkGasCostExpectation(test, _compileViaYul))
				m_gasCostFailure = true;

			test.setFailure(!m_transactionSuccessful);
			test.setRawBytes(bytes());
		}
		else
		{
			bytes output;
			if (test.call().kind == FunctionCall::Kind::LowLevel)
				output = callLowLevel(test.call().arguments.rawBytes(), test.call().value.value);
			else if (test.call().kind == FunctionCall::Kind::Builtin)
			{
				std::optional<bytes> builtinOutput = m_builtins.at(test.call().signature)(test.call());
				if (builtinOutput.has_value())
				{
					m_transactionSuccessful = true;
					output = builtinOutput.value();
				}
				else
					m_transactionSuccessful = false;
			}
			else
			{
				soltestAssert(
					m_allowNonExistingFunctions ||
					m_compiler.methodIdentifiers(m_compiler.lastContractName()).isMember(test.call().signature),
					"The function " + test.call().signature + " is not known to the compiler"
				);

				output = callContractFunctionWithValueNoEncoding(
					test.call().signature,
					test.call().value.value,
					test.call().arguments.rawBytes()
				);
			}

			bool outputMismatch = (output != test.call().expectations.rawBytes());
			if (!outputMismatch && !checkGasCostExpectation(test, _compileViaYul))
			{
				success = false;
				m_gasCostFailure = true;
			}

			// Pre byzantium, it was not possible to return failure data, so we disregard
			// output mismatch for those EVM versions.
			if (test.call().expectations.failure && !m_transactionSuccessful && !m_evmVersion.supportsReturndata())
				outputMismatch = false;
			if (m_transactionSuccessful != !test.call().expectations.failure || outputMismatch)
				success = false;

			test.setFailure(!m_transactionSuccessful);
			test.setRawBytes(std::move(output));
			test.setContractABI(m_compiler.contractABI(m_compiler.lastContractName()));
		}
	}

	if (!m_runWithYul && _compileViaYul)
	{
		m_compileViaYulCanBeSet = success;
		string message = success ?
			"Test can pass via Yul, but marked with \"compileViaYul: false.\"" :
			"Test compiles via Yul, but it gives different test results.";
		AnsiColorized(_stream, _formatted, {BOLD, success ? YELLOW : MAGENTA}) <<
			_linePrefix << endl <<
			_linePrefix << message << endl;
		return TestResult::Failure;
	}

	if (!success && (m_runWithYul || !_compileViaYul))
	{
		AnsiColorized(_stream, _formatted, {BOLD, CYAN}) << _linePrefix << "Expected result:" << endl;
		for (TestFunctionCall const& test: m_tests)
		{
			ErrorReporter errorReporter;
			_stream << test.format(
				errorReporter,
				_linePrefix,
				TestFunctionCall::RenderMode::ExpectedValuesExpectedGas,
				_formatted,
				/* _interactivePrint */ true
			) << endl;
			_stream << errorReporter.format(_linePrefix, _formatted);
		}
		_stream << endl;
		AnsiColorized(_stream, _formatted, {BOLD, CYAN}) << _linePrefix << "Obtained result:" << endl;
		for (TestFunctionCall const& test: m_tests)
		{
			ErrorReporter errorReporter;
			_stream << test.format(
				errorReporter,
				_linePrefix,
				m_gasCostFailure ? TestFunctionCall::RenderMode::ExpectedValuesActualGas : TestFunctionCall::RenderMode::ActualValuesExpectedGas,
				_formatted,
				/* _interactivePrint */ true
			) << endl;
			_stream << errorReporter.format(_linePrefix, _formatted);
		}
		AnsiColorized(_stream, _formatted, {BOLD, RED})
			<< _linePrefix << endl
			<< _linePrefix << "Attention: Updates on the test will apply the detected format displayed." << endl;
		if (_compileViaYul && m_runWithoutYul)
		{
			_stream << _linePrefix << endl << _linePrefix;
			AnsiColorized(_stream, _formatted, {RED_BACKGROUND}) << "Note that the test passed without Yul.";
			_stream << endl;
		}
		else if (!_compileViaYul && m_runWithYul)
			AnsiColorized(_stream, _formatted, {BOLD, YELLOW})
				<< _linePrefix << endl
				<< _linePrefix << "Note that the test also has to pass via Yul." << endl;
		return TestResult::Failure;
	}

	return TestResult::Success;
}

bool SemanticTest::checkGasCostExpectation(TestFunctionCall& io_test, bool _compileViaYul) const
{
	string setting =
		(_compileViaYul ? "ir"s : "legacy"s) +
		(m_optimiserSettings == OptimiserSettings::full() ? "Optimized" : "");

	// We don't check gas if enforce gas cost is not active
	// or test is run with abi encoder v1 only
	// or gas used less than threshold for enforcing feature
	// or setting is "ir" and it's not included in expectations
	if (
		!m_enforceGasCost ||
		(
			(setting == "ir" || m_gasUsed < m_enforceGasCostMinValue || m_gasUsed >= m_gas) &&
			io_test.call().expectations.gasUsed.count(setting) == 0
		)
	)
		return true;

	solAssert(!m_runWithABIEncoderV1Only, "");

	io_test.setGasCost(setting, m_gasUsed);
	return
		io_test.call().expectations.gasUsed.count(setting) > 0 &&
		m_gasUsed == io_test.call().expectations.gasUsed.at(setting);
}

void SemanticTest::printSource(ostream& _stream, string const& _linePrefix, bool _formatted) const
{
	if (m_sources.sources.empty())
		return;

	bool outputNames = (m_sources.sources.size() != 1 || !m_sources.sources.begin()->first.empty());

	for (auto const& [name, source]: m_sources.sources)
		if (_formatted)
		{
			if (source.empty())
				continue;

			if (outputNames)
				_stream << _linePrefix << formatting::CYAN << "==== Source: " << name << " ====" << formatting::RESET << endl;
			vector<char const*> sourceFormatting(source.length(), formatting::RESET);

			_stream << _linePrefix << sourceFormatting.front() << source.front();
			for (size_t i = 1; i < source.length(); i++)
			{
				if (sourceFormatting[i] != sourceFormatting[i - 1])
					_stream << sourceFormatting[i];
				if (source[i] != '\n')
					_stream << source[i];
				else
				{
					_stream << formatting::RESET << endl;
					if (i + 1 < source.length())
						_stream << _linePrefix << sourceFormatting[i];
				}
			}
			_stream << formatting::RESET;
		}
		else
		{
			if (outputNames)
				_stream << _linePrefix << "==== Source: " + name << " ====" << endl;
			stringstream stream(source);
			string line;
			while (getline(stream, line))
				_stream << _linePrefix << line << endl;
		}
}

void SemanticTest::printUpdatedExpectations(ostream& _stream, string const&) const
{
	for (TestFunctionCall const& test: m_tests)
		_stream << test.format(
			"",
			m_gasCostFailure ? TestFunctionCall::RenderMode::ExpectedValuesActualGas : TestFunctionCall::RenderMode::ActualValuesExpectedGas,
			/* _highlight = */ false
		) << endl;
}

void SemanticTest::printUpdatedSettings(ostream& _stream, string const& _linePrefix)
{
	auto& settings = m_reader.settings();
	if (settings.empty() && !m_compileViaYulCanBeSet)
		return;

	_stream << _linePrefix << "// ====" << endl;
	if (m_compileViaYulCanBeSet)
		_stream << _linePrefix << "// compileViaYul: also\n";
	for (auto const& setting: settings)
		if (!m_compileViaYulCanBeSet || setting.first != "compileViaYul")
		_stream << _linePrefix << "// " << setting.first << ": " << setting.second << endl;
}

void SemanticTest::parseExpectations(istream& _stream)
{
	TestFileParser parser{_stream, m_builtins};
	m_tests += parser.parseFunctionCalls(m_lineOffset);
}

bool SemanticTest::deploy(
	string const& _contractName,
	u256 const& _value,
	bytes const& _arguments,
	map<string, solidity::test::Address> const& _libraries
)
{
	auto output = compileAndRunWithoutCheck(m_sources.sources, _value, _contractName, _arguments, _libraries);
	return !output.empty() && m_transactionSuccessful;
}
