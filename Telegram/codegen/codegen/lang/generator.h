// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include <memory>
#include <map>
#include <set>
#include <functional>
#include <QtCore/QString>
#include <QtCore/QSet>
#include "codegen/common/cpp_file.h"
#include "codegen/lang/parsed_file.h"

namespace codegen {
namespace lang {

class Generator {
public:
	Generator(const LangPack &langpack, const QString &destBasePath, const common::ProjectInfo &project);
	Generator(const Generator &other) = delete;
	Generator &operator=(const Generator &other) = delete;

	bool writeHeader();
	bool writeSource();

private:
	void writeHeaderForwardDeclarations();
	void writeHeaderTagTypes();
	void writeHeaderInterface();
	void writeHeaderTagValueLookup();
	void writeHeaderReactiveInterface();
	void writeHeaderProducersInterface();
	void writeHeaderProducersInstances();

	QString getFullKey(const LangPack::Entry &entry);

	template <typename ComputeResult>
	void writeSetSearch(const std::set<QString, std::greater<>> &set, ComputeResult computeResult, const QString &invalidResult);

	const LangPack &langpack_;
	QString basePath_, baseName_;
	const common::ProjectInfo &project_;
	std::unique_ptr<common::CppFile> source_, header_;

};

} // namespace lang
} // namespace codegen
