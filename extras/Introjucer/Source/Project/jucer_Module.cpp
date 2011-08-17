/*
  ==============================================================================

   This file is part of the JUCE library - "Jules' Utility Class Extensions"
   Copyright 2004-11 by Raw Material Software Ltd.

  ------------------------------------------------------------------------------

   JUCE can be redistributed and/or modified under the terms of the GNU General
   Public License (Version 2), as published by the Free Software Foundation.
   A copy of the license is included in the JUCE distribution, or can be found
   online at www.gnu.org/licenses.

   JUCE is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
   A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  ------------------------------------------------------------------------------

   To release a closed-source product which uses JUCE, commercial licenses are
   available: visit www.rawmaterialsoftware.com/juce for more information.

  ==============================================================================
*/

#include "jucer_Module.h"
#include "jucer_ProjectType.h"
#include "../Project Saving/jucer_ProjectExporter.h"
#include "../Project Saving/jucer_ProjectSaver.h"
#include "jucer_AudioPluginModule.h"


//==============================================================================
ModuleList::ModuleList()
{
    rescan();
}

ModuleList& ModuleList::getInstance()
{
    static ModuleList list;
    return list;
}

struct ModuleSorter
{
    static int compareElements (const ModuleList::Module* m1, const ModuleList::Module* m2)
    {
        return m1->uid.compareIgnoreCase (m2->uid);
    }
};

void ModuleList::rescan()
{
    modules.clear();

    moduleFolder = StoredSettings::getInstance()->getLastKnownJuceFolder().getChildFile ("modules");

    DirectoryIterator iter (moduleFolder, false, "*", File::findDirectories);

    while (iter.next())
    {
        const File moduleDef (iter.getFile().getChildFile ("juce_module_info"));

        if (moduleDef.exists())
        {
            ScopedPointer<LibraryModule> m (new LibraryModule (moduleDef));
            jassert (m->isValid());

            if (m->isValid())
            {
                Module* info = new Module();
                modules.add (info);

                info->uid = m->getID();
                info->name = m->moduleInfo ["name"];
                info->description = m->moduleInfo ["description"];
                info->file = moduleDef;
            }
        }
    }

    ModuleSorter sorter;
    modules.sort (sorter);
}

LibraryModule* ModuleList::Module::create() const
{
    return new LibraryModule (file);
}

LibraryModule* ModuleList::loadModule (const String& uid) const
{
    const Module* const m = findModuleInfo (uid);

    return m != nullptr ? m->create() : nullptr;
}

const ModuleList::Module* ModuleList::findModuleInfo (const String& uid) const
{
    for (int i = modules.size(); --i >= 0;)
        if (modules.getUnchecked(i)->uid == uid)
            return modules.getUnchecked(i);

    return nullptr;
}

void ModuleList::getDependencies (const String& moduleID, StringArray& dependencies) const
{
    ScopedPointer<LibraryModule> m (loadModule (moduleID));

    if (m != nullptr)
    {
        const var depsArray (m->moduleInfo ["dependencies"]);
        const Array<var>* const deps = depsArray.getArray();

        for (int i = 0; i < deps->size(); ++i)
        {
            const var& d = deps->getReference(i);

            String uid (d ["id"].toString());
            String version (d ["version"].toString());

            if (! dependencies.contains (uid, true))
            {
                dependencies.add (uid);
                getDependencies (uid, dependencies);
            }
        }
    }
}

void ModuleList::createDependencies (const String& moduleID, OwnedArray<LibraryModule>& modules) const
{
    ScopedPointer<LibraryModule> m (loadModule (moduleID));

    if (m != nullptr)
    {
        var depsArray (m->moduleInfo ["dependencies"]);
        const Array<var>* const deps = depsArray.getArray();

        for (int i = 0; i < deps->size(); ++i)
        {
            const var& d = deps->getReference(i);

            String uid (d ["id"].toString());
            String version (d ["version"].toString());

            //xxx to do - also need to find version conflicts
            jassertfalse

        }
    }
}

//==============================================================================
LibraryModule::LibraryModule (const File& file)
    : moduleInfo (JSON::parse (file)),
      moduleFile (file),
      moduleFolder (file.getParentDirectory())
{
    jassert (isValid());
}

String LibraryModule::getID() const    { return moduleInfo ["id"].toString(); };
bool LibraryModule::isValid() const    { return getID().isNotEmpty(); }

bool LibraryModule::isPluginClient() const                          { return getID() == "juce_audio_plugin_client"; }
bool LibraryModule::isAUPluginHost (const Project& project) const   { return getID() == "juce_audio_processors" && project.isConfigFlagEnabled ("JUCE_PLUGINHOST_AU"); }
bool LibraryModule::isVSTPluginHost (const Project& project) const  { return getID() == "juce_audio_processors" && project.isConfigFlagEnabled ("JUCE_PLUGINHOST_VST"); }

File LibraryModule::getLocalIncludeFolder (ProjectSaver& projectSaver) const
{
    return projectSaver.getGeneratedCodeFolder().getChildFile ("modules").getChildFile (getID());
}

File LibraryModule::getInclude (const File& folder) const
{
    return folder.getChildFile (moduleInfo ["include"]);
}

RelativePath LibraryModule::getModuleRelativeToProject (ProjectExporter& exporter) const
{
    return RelativePath (exporter.getJuceFolder().toString(), RelativePath::projectFolder)
                .getChildFile ("modules")
                .getChildFile (getID());
}

//==============================================================================
void LibraryModule::writeIncludes (ProjectSaver& projectSaver, OutputStream& out)
{
    const File localModuleFolder (getLocalIncludeFolder (projectSaver));
    const File localHeader (getInclude (localModuleFolder));

    if (projectSaver.getProject().shouldCopyModuleFilesLocally (getID()).getValue())
    {
        moduleFolder.copyDirectoryTo (localModuleFolder);
    }
    else
    {
        localModuleFolder.createDirectory();
        createLocalHeaderWrapper (projectSaver, getInclude (moduleFolder), localHeader);
    }

    out << CodeHelpers::createIncludeStatement (localHeader, projectSaver.getGeneratedCodeFolder().getChildFile ("AppConfig.h")) << newLine;
}

static void writeGuardedInclude (OutputStream& out, StringArray paths, StringArray guards)
{
    StringArray uniquePaths (paths);
    uniquePaths.removeDuplicates (false);

    if (uniquePaths.size() == 1)
    {
        out << "#include " << paths[0] << newLine;
    }
    else
    {
        int i = paths.size();
        for (; --i >= 0;)
        {
            for (int j = i; --j >= 0;)
            {
                if (paths[i] == paths[j] && guards[i] == guards[j])
                {
                    paths.remove (i);
                    guards.remove (i);
                }
            }
        }

        for (i = 0; i < paths.size(); ++i)
        {
            out << (i == 0 ? "#if " : "#elif ") << guards[i] << newLine
                << " #include " << paths[i] << newLine;
        }

        out << "#else" << newLine
            << " #error \"This file is designed to be used in an Introjucer-generated project!\"" << newLine
            << "#endif" << newLine;
    }
}

void LibraryModule::createLocalHeaderWrapper (ProjectSaver& projectSaver, const File& originalHeader, const File& localHeader) const
{
    Project& project = projectSaver.getProject();

    MemoryOutputStream out;

    out << "// This is an auto-generated file to redirect any included" << newLine
        << "// module headers to the correct external folder." << newLine
        << newLine;

    StringArray paths, guards;
    for (int i = project.getNumExporters(); --i >= 0;)
    {
        ScopedPointer <ProjectExporter> exporter (project.createExporter (i));

        if (exporter != nullptr)
        {
            const RelativePath headerFromProject (getModuleRelativeToProject (*exporter)
                                                   .getChildFile (originalHeader.getFileName()));

            const RelativePath fileFromHere (headerFromProject.rebased (project.getFile().getParentDirectory(),
                                                                        localHeader.getParentDirectory(), RelativePath::unknown));

            paths.add (fileFromHere.toUnixStyle().quoted());
            guards.add ("defined (" + exporter->getExporterIdentifierMacro() + ")");
        }
    }

    writeGuardedInclude (out, paths, guards);
    out << newLine;

    projectSaver.replaceFileIfDifferent (localHeader, out);
}

//==============================================================================
void LibraryModule::prepareExporter (ProjectExporter& exporter, ProjectSaver& projectSaver) const
{
    Project& project = exporter.getProject();

    File localFolder (moduleFolder);
    if (project.shouldCopyModuleFilesLocally (getID()).getValue())
        localFolder = getLocalIncludeFolder (projectSaver);

    {
        Array<File> compiled;
        findAndAddCompiledCode (exporter, projectSaver, localFolder, compiled);

        if (project.shouldShowAllModuleFilesInProject (getID()).getValue())
            addBrowsableCode (exporter, compiled, localFolder);
    }

    if (isVSTPluginHost (project))
        VSTHelpers::addVSTFolderToPath (exporter, exporter.extraSearchPaths);

    if (exporter.isXcode())
    {
        if (isAUPluginHost (project))
            exporter.xcodeFrameworks.addTokens ("AudioUnit CoreAudioKit", false);

        const String frameworks (moduleInfo [exporter.isOSX() ? "OSXFrameworks" : "iOSFrameworks"].toString());
        exporter.xcodeFrameworks.addTokens (frameworks, ", ", String::empty);
    }

    if (isPluginClient())
    {
        if (shouldBuildVST  (project).getValue())  VSTHelpers::prepareExporter (exporter, projectSaver);
        if (shouldBuildRTAS (project).getValue())  RTASHelpers::prepareExporter (exporter, projectSaver, localFolder);
        if (shouldBuildAU   (project).getValue())  AUHelpers::prepareExporter (exporter, projectSaver);
    }
}

void LibraryModule::createPropertyEditors (const ProjectExporter& exporter, Array <PropertyComponent*>& props) const
{
    if (isVSTPluginHost (exporter.getProject()))
        VSTHelpers::createVSTPathEditor (exporter, props);

    if (isPluginClient())
    {
        if (shouldBuildVST  (exporter.getProject()).getValue())  VSTHelpers::createPropertyEditors (exporter, props);
        if (shouldBuildRTAS (exporter.getProject()).getValue())  RTASHelpers::createPropertyEditors (exporter, props);
    }
}

void LibraryModule::getConfigFlags (Project& project, OwnedArray<Project::ConfigFlag>& flags) const
{
    const File header (getInclude (moduleFolder));
    jassert (header.exists());

    StringArray lines;
    header.readLines (lines);

    for (int i = 0; i < lines.size(); ++i)
    {
        String line (lines[i].trim());

        if (line.startsWith ("/**") && line.containsIgnoreCase ("Config:"))
        {
            ScopedPointer <Project::ConfigFlag> config (new Project::ConfigFlag());
            config->sourceModuleID = getID();
            config->symbol = line.fromFirstOccurrenceOf (":", false, false).trim();

            if (config->symbol.length() > 2)
            {
                ++i;

                while (! (lines[i].contains ("*/") || lines[i].contains ("@see")))
                {
                    if (lines[i].trim().isNotEmpty())
                        config->description = config->description.trim() + " " + lines[i].trim();

                    ++i;
                }

                config->description = config->description.upToFirstOccurrenceOf ("*/", false, false);
                config->value.referTo (project.getConfigFlag (config->symbol));
                flags.add (config.release());
            }
        }
    }
}

//==============================================================================
bool LibraryModule::fileTargetMatches (ProjectExporter& exporter, const String& target)
{
    if (target.startsWithChar ('!'))
        return ! fileTargetMatches (exporter, target.substring (1).trim());

    if (target == "xcode")  return exporter.isXcode();
    if (target == "msvc")   return exporter.isVisualStudio();
    if (target == "linux")  return exporter.isLinux();

    return true;
}

void LibraryModule::findWildcardMatches (const File& localModuleFolder, const String& wildcardPath, Array<File>& result) const
{
    String path (wildcardPath.upToLastOccurrenceOf ("/", false, false));
    String wildCard (wildcardPath.fromLastOccurrenceOf ("/", false, false));

    Array<File> tempList;
    FileSorter sorter;

    DirectoryIterator iter (localModuleFolder.getChildFile (path), false, wildCard);
    while (iter.next())
        tempList.addSorted (sorter, iter.getFile());

    result.addArray (tempList);
}

void LibraryModule::addFileWithGroups (Project::Item& group, const RelativePath& file, const String& path) const
{
    const int slash = path.indexOfChar ('/');

    if (slash >= 0)
    {
        const String topLevelGroup (path.substring (0, slash));
        const String remainingPath (path.substring (slash + 1));

        Project::Item newGroup (group.getOrCreateSubGroup (topLevelGroup));
        addFileWithGroups (newGroup, file, remainingPath);
    }
    else
    {
        if (! group.containsChildForFile (file))
            group.addRelativeFile (file, -1, false);
    }
}

void LibraryModule::findAndAddCompiledCode (ProjectExporter& exporter, ProjectSaver& projectSaver,
                                            const File& localModuleFolder, Array<File>& result) const
{
    const var compileArray (moduleInfo ["compile"]); // careful to keep this alive while the array is in use!
    const Array<var>* const files = compileArray.getArray();

    if (files != nullptr)
    {
        for (int i = 0; i < files->size(); ++i)
        {
            const var& file = files->getReference(i);
            const String filename (file ["file"].toString());

            if (filename.isNotEmpty()
                 && fileTargetMatches (exporter, file ["target"].toString()))
            {
                const File compiledFile (localModuleFolder.getChildFile (filename));
                result.add (compiledFile);

                Project::Item item (projectSaver.addFileToGeneratedGroup (compiledFile));

                if (file ["warnings"].toString().equalsIgnoreCase ("disabled"))
                    item.getShouldInhibitWarningsValue() = true;

                if (file ["stdcall"])
                    item.getShouldUseStdCallValue() = true;
            }
        }
    }
}

void LibraryModule::addBrowsableCode (ProjectExporter& exporter, const Array<File>& compiled, const File& localModuleFolder) const
{
    if (sourceFiles.size() == 0)
    {
        const var filesArray (moduleInfo ["browse"]);
        const Array<var>* const files = filesArray.getArray();

        for (int i = 0; i < files->size(); ++i)
            findWildcardMatches (localModuleFolder, files->getReference(i), sourceFiles);
    }

    Project::Item sourceGroup (Project::Item::createGroup (exporter.getProject(), getID(), "__mainsourcegroup" + getID()));

    const RelativePath moduleFromProject (getModuleRelativeToProject (exporter));

    for (int i = 0; i < sourceFiles.size(); ++i)
    {
        const String pathWithinModule (sourceFiles.getReference(i).getRelativePathFrom (localModuleFolder));

        addFileWithGroups (sourceGroup,
                           moduleFromProject.getChildFile (pathWithinModule),
                           pathWithinModule);
    }

    sourceGroup.addFile (localModuleFolder.getChildFile (moduleFile.getRelativePathFrom (moduleFolder)), -1, false);
    sourceGroup.addFile (getInclude (localModuleFolder), -1, false);

    exporter.getModulesGroup().getNode().addChild (sourceGroup.getNode().createCopy(), -1, nullptr);
}
