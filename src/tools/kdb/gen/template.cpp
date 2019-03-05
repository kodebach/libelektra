/**
 * @file
 *
 * @brief
 *
 * @copyright BSD License (see LICENSE.md or https://www.libelektra.org)
 */

#include "template.hpp"

#include <algorithm>
#include <fstream>

#include "gen/templates.hpp"

#include "elektragen.hpp"

GenTemplate::GenTemplate (std::string templateBaseName, std::vector<std::string> parts,
			  const std::unordered_map<std::string, bool> & parameters)
: _templateBaseName (std::move (templateBaseName)), _parts (std::move (parts)), _parameters (), _requiredParameters ()
{
	std::for_each (parameters.begin (), parameters.end (), [this](const std::pair<std::string, bool> & p) {
		_parameters[p.first] = "";
		if (p.second)
		{
			_requiredParameters.insert (p.first);
		}
	});
}

void GenTemplate::render (std::ostream & output, const std::string & outputName, const std::string & part, const kdb::KeySet & ks,
			  const std::string & parentKey) const
{
	using namespace kainjow::mustache;

	auto missingParam = std::find_if (_requiredParameters.begin (), _requiredParameters.end (),
					  [this](const std::string & p) { return getParameter (p).empty (); });

	if (missingParam != _requiredParameters.end ())
	{
		throw std::runtime_error ("missing required parameter " + *missingParam);
	}

	if (std::find (_parts.begin (), _parts.end (), part) == _parts.end ())
	{
		return;
	}

	auto name = _templateBaseName + part;
	std::replace_if (name.begin (), name.end (), std::not1 (std::ptr_fun (isalnum)), '_');

	auto tmpl = mustache (kdbgenTemplates.at (name));
	tmpl.render (getTemplateData (outputName, ks, parentKey), [&](const std::string & str) { output << str; });
}

std::string GenTemplate::getParameter (const std::string & name, const std::string & defaultValue) const
{
	auto search = _parameters.find (name);
	auto param = search != _parameters.end () ? search->second : "";
	return param.empty () ? defaultValue : param;
}

void GenTemplate::setParameter (const std::string & name, const std::string & value)
{
	auto param = _parameters.find (name);
	if (param != _parameters.end ())
	{
		param->second = value;
	}
}

void GenTemplate::clearParameters ()
{
	std::for_each (_parameters.begin (), _parameters.end (),
		       [this](const std::pair<std::string, std::string> & param) { _parameters[param.first] = ""; });
}

std::vector<std::string> GenTemplate::getParts () const
{
	return _parts;
}

template <class genClass>
void GenTemplateList::addTemplate (const std::string & name)
{
	std::unique_ptr<GenTemplate> tmpl (new genClass ());
	_templates[name] = std::move (tmpl);
}

const GenTemplate * GenTemplateList::getTemplate (const std::string & name,
						  const std::unordered_map<std::string, std::string> & parameters) const
{
	auto search = _templates.find (name);
	if (search == _templates.end ())
	{
		return &EmptyGenTemplate::getInstance ();
	}

	auto tmpl = search->second.get ();
	tmpl->clearParameters ();
	std::for_each (parameters.begin (), parameters.end (),
		       [tmpl](const std::pair<std::string, std::string> & param) { tmpl->setParameter (param.first, param.second); });
	return tmpl;
}

GenTemplateList::GenTemplateList () : _templates ()
{
	addTemplate<ElektraGenTemplate> ("elektra");
}