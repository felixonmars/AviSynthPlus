#include "PluginManager.h"
#include <avisynth.h>
#include <avisynth_c.h>
#include "strings.h"
#include <cassert>

typedef const char* (__stdcall *AvisynthPluginInit3Func)(IScriptEnvironment* env, const AVS_Linkage* const vectors);
typedef const char* (__stdcall *AvisynthPluginInit2Func)(IScriptEnvironment* env);
typedef const char* (AVSC_CC *AvisynthCPluginInitFunc)(AVS_ScriptEnvironment* env);

const char RegAvisynthKey[] = "Software\\Avisynth";
const char RegPluginDirClassic[] = "PluginDir2_5";
const char RegPluginDirPlus[] = "PluginDir+";

/*
---------------------------------------------------------------------------------
---------------------------------------------------------------------------------
                                 Static helpers
---------------------------------------------------------------------------------
---------------------------------------------------------------------------------
*/

// Translates a Windows error code to a human-readable text message.
static std::string GetLastErrorText(DWORD nErrorCode)
{
  char* msg;
  // Ask Windows to prepare a standard message for a GetLastError() code:
  if (0 == FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, nErrorCode, 0, (LPSTR)&msg, 0, NULL))
    return("Unknown error");
  else
  {
    std::string ret(msg);
    LocalFree(msg);
    return ret;
  }
}

static bool GetRegString(HKEY rootKey, const char path[], const char entry[], std::string *result) {
    HKEY AvisynthKey;

    if (RegOpenKeyEx(rootKey, path, 0, KEY_READ, &AvisynthKey))
      return false;

    DWORD size;
    if (ERROR_SUCCESS != RegQueryValueEx(AvisynthKey, entry, 0, 0, 0, &size)) {
      RegCloseKey(AvisynthKey); // Dave Brueck - Dec 2005
      return false;
    }

    char* retStr = new(std::nothrow) char[size];
    if ((retStr == NULL) || (ERROR_SUCCESS != RegQueryValueEx(AvisynthKey, entry, 0, 0, (LPBYTE)retStr, &size))) {
      delete[] retStr;
      RegCloseKey(AvisynthKey); // Dave Brueck - Dec 2005
      return false;
    }
    RegCloseKey(AvisynthKey); // Dave Brueck - Dec 2005

    *result = std::string(retStr);
    delete[] retStr;
    return true;
}

static std::string GetFullPathNameWrap(const std::string &f)
{
  // Get the lenght of the buffer we need
  DWORD len = GetFullPathName(f.c_str(), 0, NULL, NULL);

  // Reserve space and make call
  char *fullPathName = new char[len];
  GetFullPathName(f.c_str(), len, fullPathName, NULL);

  // Wrap into C++ string
  std::string result(fullPathName);

  // Cleanup and return
  delete [] fullPathName;
  return result;
}

static bool IsParameterTypeSpecifier(char c) {
  switch (c) {
    case 'b': case 'i': case 'f': case 's': case 'c': case '.':
      return true;
    default:
      return false;
  }
}

static bool IsParameterTypeModifier(char c) {
  switch (c) {
    case '+': case '*':
      return true;
    default:
      return false;
  }
}

static bool IsValidParameterString(const char* p) {
  int state = 0;
  char c;
  while ((c = *p++) != '\0' && state != -1) {
    switch (state) {
      case 0:
        if (IsParameterTypeSpecifier(c)) {
          state = 1;
        }
        else if (c == '[') {
          state = 2;
        }
        else {
          state = -1;
        }
        break;

      case 1:
        if (IsParameterTypeSpecifier(c)) {
          // do nothing; stay in the current state
        }
        else if (c == '[') {
          state = 2;
        }
        else if (IsParameterTypeModifier(c)) {
          state = 0;
        }
        else {
          state = -1;
        }
        break;

      case 2:
        if (c == ']') {
          state = 3;
        }
        else {
          // do nothing; stay in the current state
        }
        break;

      case 3:
        if (IsParameterTypeSpecifier(c)) {
          state = 1;
        }
        else {
          state = -1;
        }
        break;
    }
  }

  // states 0, 1 are the only ending states we accept
  return state == 0 || state == 1;
}

/*
---------------------------------------------------------------------------------
---------------------------------------------------------------------------------
                                 AVSFunction
---------------------------------------------------------------------------------
---------------------------------------------------------------------------------
*/

AVSFunction::AVSFunction() : 
    AVSFunction(NULL, NULL, NULL, NULL, NULL)
{}

AVSFunction::AVSFunction(void*) : 
    AVSFunction(NULL, NULL, NULL, NULL, NULL)
{}

AVSFunction::AVSFunction(const char* _name, const char* _plugin_basename, const char* _param_types, apply_func_t _apply) :
    AVSFunction(_name, _plugin_basename, _param_types, _apply, NULL)
{}

AVSFunction::AVSFunction(const char* _name, const char* _plugin_basename, const char* _param_types, apply_func_t _apply, void *_user_data) :
    apply(_apply), name(NULL), canon_name(NULL), param_types(NULL), user_data(_user_data)
{
    if ( NULL != _name )
    {
        size_t len = strlen(_name);
        name = new char[len+1];
        memcpy(name, _name, len);
        name[len] = 0;
    }

    if ( NULL != _param_types )
    {
        size_t len = strlen(_param_types);
        param_types = new char[len+1];
        memcpy(param_types, _param_types, len);
        param_types[len] = 0;
    }

    if ( (NULL != _name) && (NULL != _plugin_basename) )
    {
        std::string cn(_plugin_basename);
        cn.append("_").append(_name);
        canon_name = new char[cn.size()+1];
        memcpy(canon_name, cn.c_str(), cn.size());
        canon_name[cn.size()] = 0;
    }
}

AVSFunction::~AVSFunction()
{
    delete [] canon_name;
    delete [] name;
    delete [] param_types;
}

AVSFunction::AVSFunction(const AVSFunction &obj) :
    apply(obj.apply), name(NULL), canon_name(NULL), param_types(NULL), user_data(obj.user_data)
{
    if ( NULL != obj.name )
    {
        size_t len = strlen(obj.name);
        name = new char[len+1];
        memcpy(name, obj.name, len);
        name[len] = 0;
    }

    if ( NULL != obj.param_types )
    {
        size_t len = strlen(obj.param_types);
        param_types = new char[len+1];
        memcpy(param_types, obj.param_types, len);
        param_types[len] = 0;
    }

    if (NULL != obj.canon_name)
    {
        size_t len = strlen(obj.canon_name);
        canon_name = new char[len+1];
        memcpy(canon_name, obj.canon_name, len);
        canon_name[len] = 0;
    }
}

AVSFunction & AVSFunction::operator=(const AVSFunction &rhs)
{
    if(this == &rhs)
       return *this;

    delete[] canon_name;
    canon_name = NULL;
    delete[] name;
    name = NULL;
    delete[] param_types;
    param_types = NULL;

    apply = rhs.apply;
    name = NULL;
    canon_name = NULL;
    param_types = NULL;
    user_data = rhs.user_data;

    if ( NULL != rhs.name )
    {
        size_t len = strlen(rhs.name);
        name = new char[len+1];
        memcpy(name, rhs.name, len);
        name[len] = 0;
    }

    if ( NULL != rhs.param_types )
    {
        size_t len = strlen(rhs.param_types);
        param_types = new char[len+1];
        memcpy(param_types, rhs.param_types, len);
        param_types[len] = 0;
    }

    if (NULL != rhs.canon_name)
    {
        size_t len = strlen(rhs.canon_name);
        canon_name = new char[len+1];
        memcpy(canon_name, rhs.canon_name, len);
        canon_name[len] = 0;
    }

    return *this;
}

AVSFunction::AVSFunction(AVSFunction &&obj) :
    name(NULL), canon_name(NULL), param_types(NULL)
{
    *this = std::move(obj);
}

AVSFunction & AVSFunction::operator=(AVSFunction &&rhs)
{
    if (this == &rhs)
        return *this;

    delete[] canon_name;
    delete[] name;
    delete[] param_types;

    apply = rhs.apply;
    name = rhs.name;
    canon_name = rhs.canon_name;
    param_types = rhs.param_types;
    user_data = rhs.user_data;

    rhs.apply = NULL;
    rhs.name = NULL;
    rhs.canon_name = NULL;
    rhs.param_types = NULL;
    rhs.user_data = NULL;

    return *this;
}

bool AVSFunction::empty() const
{
    return NULL == name;
}

bool AVSFunction::IsScriptFunction() const
{
    return apply == &(ScriptFunction::Execute);
}

bool AVSFunction::SingleTypeMatch(char type, const AVSValue& arg, bool strict) {
  switch (type) {
    case '.': return true;
    case 'b': return arg.IsBool();
    case 'i': return arg.IsInt();
    case 'f': return arg.IsFloat() && (!strict || !arg.IsInt());
    case 's': return arg.IsString();
    case 'c': return arg.IsClip();
    default:  return false;
  }
}

bool AVSFunction::TypeMatch(const char* param_types, const AVSValue* args, size_t num_args, bool strict, IScriptEnvironment* env) {

  bool optional = false;

  size_t i = 0;
  while (i < num_args) {

    if (*param_types == '\0') {
      // more args than params
      return false;
    }

    if (*param_types == '[') {
      // named arg: skip over the name
      param_types = strchr(param_types+1, ']');
      if (param_types == NULL) {
        env->ThrowError("TypeMatch: unterminated parameter name (bug in filter)");
      }

      ++param_types;
      optional = true;

      if (*param_types == '\0') {
        env->ThrowError("TypeMatch: no type specified for optional parameter (bug in filter)");
      }
    }

    if (param_types[1] == '*') {
      // skip over initial test of type for '*' (since zero matches is ok)
      ++param_types;
    }

    switch (*param_types) {
      case 'b': case 'i': case 'f': case 's': case 'c':
        if (   (!optional || args[i].Defined())
            && !SingleTypeMatch(*param_types, args[i], strict))
          return false;
        // fall through
      case '.':
        ++param_types;
        ++i;
        break;
      case '+': case '*':
        if (!SingleTypeMatch(param_types[-1], args[i], strict)) {
          // we're done with the + or *
          ++param_types;
        }
        else {
          ++i;
        }
        break;
      default:
        env->ThrowError("TypeMatch: invalid character in parameter list (bug in filter)");
    }
  }

  // We're out of args.  We have a match if one of the following is true:
  // (a) we're out of params.
  // (b) remaining params are named i.e. optional.
  // (c) we're at a '+' or '*' and any remaining params are optional.

  if (*param_types == '+'  || *param_types == '*')
    param_types += 1;

  if (*param_types == '\0' || *param_types == '[')
    return true;

  while (param_types[1] == '*') {
    param_types += 2;
    if (*param_types == '\0' || *param_types == '[')
      return true;
  }

  return false;
}

bool AVSFunction::ArgNameMatch(const char* param_types, size_t args_names_count, const char* const* arg_names) {

  for (size_t i=0; i<args_names_count; ++i) {
    if (arg_names[i]) {
      bool found = false;
      size_t len = strlen(arg_names[i]);
      for (const char* p = param_types; *p; ++p) {
        if (*p == '[') {
          p += 1;
          const char* q = strchr(p, ']');
          if (!q) return false;
          if (len == q-p && !_strnicmp(arg_names[i], p, q-p)) {
            found = true;
            break;
          }
          p = q+1;
        }
      }
      if (!found) return false;
    }
  }
  return true;
}

/*
---------------------------------------------------------------------------------
---------------------------------------------------------------------------------
                                 PluginFile
---------------------------------------------------------------------------------
---------------------------------------------------------------------------------
*/

#include <avs/win.h>
struct PluginFile
{
  std::string FilePath;             // Fully qualified, canonical file path
  std::string BaseName;             // Only file name, without extension
  HMODULE Library;                  // LoadLibrary handle

  PluginFile(const std::string &filePath);
};

PluginFile::PluginFile(const std::string &filePath) : 
  FilePath(GetFullPathNameWrap(filePath)), BaseName(), Library(NULL)
{
  // Turn all '\' into '/'
  replace(FilePath, '\\', '/');

  // Find position of dot in extension
  size_t dot_pos = FilePath.rfind('.');

  // Find position of last directory slash
  size_t slash_pos = FilePath.rfind('/');

  // Extract basename
  if ((dot_pos != std::string::npos) && (slash_pos != std::string::npos))
  {// we have both a slash and a dot
    if (dot_pos > slash_pos)
      BaseName = FilePath.substr(slash_pos+1, dot_pos - slash_pos - 1);
    else
      BaseName = FilePath.substr(slash_pos+1, std::string::npos);
  }
  else if ((dot_pos == std::string::npos) && (slash_pos != std::string::npos))
  {// we have a slash but no dot
    // Extract basename
    BaseName = FilePath.substr(slash_pos+1, std::string::npos);
  }
  else
  {// everything else
    // Because we have used GetFullPathName, FilePath should contain an absolute path,
    // meaning that this case should be unreachable, but the devil never sleeps.
    assert(0);
    BaseName = FilePath;
  }
}

/*
---------------------------------------------------------------------------------
---------------------------------------------------------------------------------
                                 PluginManager
---------------------------------------------------------------------------------
---------------------------------------------------------------------------------
*/

PluginManager::PluginManager(IScriptEnvironment2* env) :
  Env(env), PluginInLoad(NULL), AutoloadExecuted(false), Autoloading(false)
{
  env->SetGlobalVar("$PluginFunctions$", AVSValue(""));
}

void PluginManager::ClearAutoloadDirs()
{
  if (AutoloadExecuted)
    Env->ThrowError("Cannot modify directory list after the autoload procedure has already executed.");

  AutoloadDirs.clear();
}

void PluginManager::AddAutoloadDir(const std::string &dirPath, bool toFront)
{
  if (AutoloadExecuted)
    Env->ThrowError("Cannot modify directory list after the autoload procedure has already executed.");

  std::string dir(dirPath);

  // get folder of our executable
  TCHAR ExeFilePath[AVS_MAX_PATH];
  memset(ExeFilePath, 0, sizeof(ExeFilePath[0])*AVS_MAX_PATH);  // WinXP does not terminate the result of GetModuleFileName with a zero, so me must zero our buffer
  GetModuleFileName(NULL, ExeFilePath, AVS_MAX_PATH);
  std::string ExeFileDir(ExeFilePath);
  replace(ExeFileDir, '\\', '/');
  ExeFileDir = ExeFileDir.erase(ExeFileDir.rfind('/'), std::string::npos);

  // variable expansion
  replace_beginning(dir, "SCRIPTDIR", Env->GetVar("$ScriptDir$", ""));
  replace_beginning(dir, "MAINSCRIPTDIR", Env->GetVar("$MainScriptDir$", ""));
  replace_beginning(dir, "PROGRAMDIR", ExeFileDir);

  std::string plugin_dir;
  if (GetRegString(HKEY_CURRENT_USER, RegAvisynthKey, RegPluginDirPlus, &plugin_dir))
    replace_beginning(dir, "USER_PLUS_PLUGINS", plugin_dir);
  if (GetRegString(HKEY_LOCAL_MACHINE, RegAvisynthKey, RegPluginDirPlus, &plugin_dir))
    replace_beginning(dir, "MACHINE_PLUS_PLUGINS", plugin_dir);
  if (GetRegString(HKEY_CURRENT_USER, RegAvisynthKey, RegPluginDirClassic, &plugin_dir))
    replace_beginning(dir, "USER_CLASSIC_PLUGINS", plugin_dir);
  if (GetRegString(HKEY_LOCAL_MACHINE, RegAvisynthKey, RegPluginDirClassic, &plugin_dir))
    replace_beginning(dir, "MACHINE_CLASSIC_PLUGINS", plugin_dir);

  // replace backslashes with forward slashes
  replace(dir, '\\', '/');

  // append terminating slash if needed
  if (dir[dir.size()-1] != '/')
    dir.append("/");

  // remove double slashes
  while(replace(dir, "//", "/"));

  if (toFront)
    AutoloadDirs.insert(AutoloadDirs.begin(), GetFullPathNameWrap(dir));
  else
    AutoloadDirs.push_back(GetFullPathNameWrap(dir));
}

void PluginManager::AutoloadPlugins()
{
  if (AutoloadExecuted)
    return;

  AutoloadExecuted = true;
  Autoloading = true;

  const char *binaryFilter = "*.dll";
  const char *scriptFilter = "*.avsi";

  // Load binary plugins
  for (const std::string& dir : AutoloadDirs)
  {
    // Append file search filter to directory path
    std::string filePattern = concat(dir, binaryFilter);

    // Iterate through all files in directory
    WIN32_FIND_DATA fileData;
    HANDLE hFind = FindFirstFile(filePattern.c_str(), &fileData);
    for (BOOL bContinue = (hFind != INVALID_HANDLE_VALUE);
          bContinue;
          bContinue = FindNextFile(hFind, &fileData)
        )
    {
      if ((fileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)  // do not add directories
      {
        PluginFile p(concat(dir, fileData.cFileName));

        // Search for loaded plugins with the same base name.
        for (size_t i = 0; i < AutoLoadedPlugins.size(); ++i)
        {
          if (streqi(AutoLoadedPlugins[i].BaseName.c_str(), p.BaseName.c_str()))
          {
            // Prevent loading a plugin with a basename that is 
            // already loaded (from another autoload folder).
            continue;
          }
        }

        // Try to load plugin
        AVSValue dummy;
        LoadPlugin(p, false, &dummy);
      }
    } // for bContinue
    FindClose(hFind);
  }

  // Load script imports
  for (const std::string& dir : AutoloadDirs)
  {
    CWDChanger cwdchange(dir.c_str());

    // Append file search filter to directory path
    std::string filePattern = concat(dir, scriptFilter);

    // Iterate through all files in directory
    WIN32_FIND_DATA fileData;
    HANDLE hFind = FindFirstFile(filePattern.c_str(), &fileData);
    for (BOOL bContinue = (hFind != INVALID_HANDLE_VALUE);
          bContinue;
          bContinue = FindNextFile(hFind, &fileData)
        )
    {
      if ((fileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)  // do not add directories
      {
        PluginFile p(concat(dir, fileData.cFileName));

        // Search for loaded imports with the same base name.
        for (size_t i = 0; i < AutoLoadedImports.size(); ++i)
        {
          if (streqi(AutoLoadedImports[i].BaseName.c_str(), p.BaseName.c_str()))
          {
            // Prevent loading a plugin with a basename that is 
            // already loaded (from another autoload folder).
            continue;
          }
        }

        // Try to load script
        Env->Invoke("Import", p.FilePath.c_str());
        AutoLoadedImports.push_back(p);
      }
    } // for bContinue
    FindClose(hFind);
  }

  Autoloading = false;
}

PluginManager::~PluginManager()
{
  for (size_t i = 0; i < LoadedPlugins.size(); ++i)
  {
    assert(LoadedPlugins[i].Library);
    FreeLibrary(LoadedPlugins[i].Library);
    LoadedPlugins[i].Library = NULL;
  }
  for (size_t i = 0; i < AutoLoadedPlugins.size(); ++i)
  {
    assert(AutoLoadedPlugins[i].Library);
    FreeLibrary(AutoLoadedPlugins[i].Library);
    AutoLoadedPlugins[i].Library = NULL;
  }
  Env = NULL;
  PluginInLoad = NULL;
}

void PluginManager::UpdateFunctionExports(const char* funcName, const char* funcParams, const char *exportVar)
{
  if (exportVar == NULL)
    exportVar = "$PluginFunctions$";

  // Update $PluginFunctions$
  const char *oldFnList = Env->GetVar(exportVar, "");
  std::string FnList(oldFnList);
  if (FnList.size() > 0)    // if the list is not empty...
    FnList.push_back(' ');  // ...add a delimiting whitespace 
  FnList.append(funcName);
  Env->SetGlobalVar(exportVar, AVSValue( Env->SaveString(FnList.c_str(), FnList.size()) ));

  // Update $Plugin!...!Param$
  std::string param_id;
  param_id.reserve(128);
  param_id.append("$Plugin!");
  param_id.append(funcName);
  param_id.append("!Param$");
  Env->SetGlobalVar(Env->SaveString(param_id.c_str(), param_id.size()), AVSValue(Env->SaveString(funcParams)));
}

bool PluginManager::LoadPlugin(const char* path, bool throwOnError, AVSValue *result)
{
  return LoadPlugin(PluginFile(path), throwOnError, result);
}

bool PluginManager::LoadPlugin(PluginFile &plugin, bool throwOnError, AVSValue *result)
{
  std::vector<PluginFile>& PluginList = Autoloading ? AutoLoadedPlugins : LoadedPlugins;

  for (size_t i = 0; i < PluginList.size(); ++i)
  {
    if (streqi(PluginList[i].FilePath.c_str(), plugin.FilePath.c_str()))
    {
      // Imitate successful loading if the plugin is already loaded
      plugin = PluginList[i];
      return true;
    }
  }

  // Search for dependent DLLs in the plugin's directory too
  size_t slash_pos = plugin.FilePath.rfind('/');
  std::string plugin_dir = plugin.FilePath.substr(0, slash_pos);;
  DllDirChanger dllchange(plugin_dir.c_str());

  // Load the dll into memory
  plugin.Library = LoadLibrary(plugin.FilePath.c_str());
  if (plugin.Library == NULL)
  {
    if (throwOnError)
    {
      DWORD errCode = GetLastError();
      Env->ThrowError("Cannot load file '%s'. Platform returned code %d:\n%s", plugin.FilePath.c_str(), errCode, GetLastErrorText(errCode).c_str());
    }
    else
      return false;
  }

  // Try to load various plugin interfaces
  if (!TryAsAvs26(plugin, result))
  {
    if (!TryAsAvsC(plugin, result))
    {
      if (!TryAsAvs25(plugin, result))
      {
        FreeLibrary(plugin.Library);
        plugin.Library = NULL;

        if (throwOnError)
          Env->ThrowError("'%s' cannot be used as a plugin for AviSynth.", plugin.FilePath.c_str());
        else
          return false;
      }
    }
  }

  PluginList.push_back(plugin);
  return true;
}

const AVSFunction* PluginManager::Lookup(const FunctionMap& map, const char* search_name, const AVSValue* args, size_t num_args,
                    bool strict, size_t args_names_count, const char* const* arg_names) const
{
    FunctionMap::const_iterator list_it = map.find(search_name);
    if (list_it == map.end())
      return NULL;

    for ( FunctionList::const_reverse_iterator func_it = list_it->second.rbegin();
          func_it != list_it->second.rend();
          ++func_it)
    {
      const AVSFunction *func = &(*func_it);
      if (AVSFunction::TypeMatch(func->param_types, args, num_args, strict, Env) &&
          AVSFunction::ArgNameMatch(func->param_types, args_names_count, arg_names)
         )
      {
        return func;
      }
    }

    return NULL;
}

const AVSFunction* PluginManager::Lookup(const char* search_name, const AVSValue* args, size_t num_args,
                    bool strict, size_t args_names_count, const char* const* arg_names) const
{
  /* Lookup in non-autoloaded functions first, so that they take priority */
  const AVSFunction* func = Lookup(ExternalFunctions, search_name, args, num_args, strict, args_names_count, arg_names);
  if (func != NULL)
    return func;

  /* If not found, look amongst the autoloaded */
  return Lookup(AutoloadedFunctions, search_name, args, num_args, strict, args_names_count, arg_names);
}

bool PluginManager::FunctionExists(const char* name) const
{
    bool autoloaded = (AutoloadedFunctions.find(name) != AutoloadedFunctions.end());
    return autoloaded || (ExternalFunctions.find(name) != ExternalFunctions.end());
}

void PluginManager::AddFunction(const char* name, const char* params, IScriptEnvironment::ApplyFunc apply, void* user_data, const char *exportVar)
{
  if (!IsValidParameterString(params))
    Env->ThrowError("%s has an invalid parameter string (bug in filter)", name);

  FunctionMap& functions = Autoloading ? AutoloadedFunctions : ExternalFunctions;

  AVSFunction newFunc;
  if (PluginInLoad != NULL)
  {
      newFunc = AVSFunction(name, PluginInLoad->BaseName.c_str(), params, apply, user_data);
  }
  else
  {
      newFunc = AVSFunction(name, NULL, params, apply, user_data);
      assert(newFunc.IsScriptFunction());
  }
  FunctionList& list = functions[newFunc.name];
  list.push_back(newFunc);
  UpdateFunctionExports(newFunc.name, newFunc.param_types, exportVar);

  if (NULL != newFunc.canon_name)
  {
      FunctionList& baseList = functions[newFunc.canon_name];
      baseList.push_back(newFunc);
      UpdateFunctionExports(newFunc.canon_name, newFunc.param_types, exportVar);
  }
}

std::string PluginManager::PluginLoading() const
{
    if (NULL == PluginInLoad)
        return std::string();
    else
        return PluginInLoad->BaseName;
}

bool PluginManager::TryAsAvs26(PluginFile &plugin, AVSValue *result)
{
  extern const AVS_Linkage* const AVS_linkage; // In interface.cpp

  AvisynthPluginInit3Func AvisynthPluginInit3 = (AvisynthPluginInit3Func)GetProcAddress(plugin.Library, "AvisynthPluginInit3");
  if (!AvisynthPluginInit3)
    AvisynthPluginInit3 = (AvisynthPluginInit3Func)GetProcAddress(plugin.Library, "_AvisynthPluginInit3@8");

  if (AvisynthPluginInit3 == NULL)
    return false;
  else
  {
    PluginInLoad = &plugin;
    *result = AvisynthPluginInit3(Env, AVS_linkage);
    PluginInLoad = NULL;
  }

  return true;
}

bool PluginManager::TryAsAvs25(PluginFile &plugin, AVSValue *result)
{
  AvisynthPluginInit2Func AvisynthPluginInit2 = (AvisynthPluginInit2Func)GetProcAddress(plugin.Library, "AvisynthPluginInit2");
  if (!AvisynthPluginInit2)
    AvisynthPluginInit2 = (AvisynthPluginInit2Func)GetProcAddress(plugin.Library, "_AvisynthPluginInit2@4");

  if (AvisynthPluginInit2 == NULL)
    return false;
  else
  {
    PluginInLoad = &plugin;
    *result = AvisynthPluginInit2(Env);
    PluginInLoad = NULL;
  }

  return true;
}

bool PluginManager::TryAsAvsC(PluginFile &plugin, AVSValue *result)
{
  AvisynthCPluginInitFunc AvisynthCPluginInit = (AvisynthCPluginInitFunc)GetProcAddress(plugin.Library, "_avisynth_c_plugin_init@4");
  if (!AvisynthCPluginInit)
    AvisynthCPluginInit = (AvisynthCPluginInitFunc)GetProcAddress(plugin.Library, "avisynth_c_plugin_init@4");

  if (AvisynthCPluginInit == NULL)
    return false;
  else
  {
    PluginInLoad = &plugin;
    {
        AVS_ScriptEnvironment e;
        e.env = Env;
        AVS_ScriptEnvironment *pe;
        pe = &e;
        const char *s = NULL;
#ifdef X86_32
        int callok = 1; // (stdcall)
        __asm // Tritical - Jan 2006
        {
            push eax
            push edx

            push 0x12345678		// Stash a known value

            mov eax, pe			// Env pointer
            push eax			// Arg1
            call AvisynthCPluginInit			// avisynth_c_plugin_init

            lea edx, s			// return value is in eax
            mov DWORD PTR[edx], eax

            pop eax				// Get top of stack
            cmp eax, 0x12345678	// Was it our known value?
            je end				// Yes! Stack was cleaned up, was a stdcall

            lea edx, callok
            mov BYTE PTR[edx], 0 // Set callok to 0 (_cdecl)

            pop eax				// Get 2nd top of stack
            cmp eax, 0x12345678	// Was this our known value?
            je end				// Yes! Stack is now correctly cleaned up, was a _cdecl

            mov BYTE PTR[edx], 2 // Set callok to 2 (bad stack)
    end:
            pop edx
            pop eax
        }
      switch(callok)
      {
      case 0:   // cdecl
#ifdef AVSC_USE_STDCALL
            Env->ThrowError("Avisynth 2 C Plugin '%s' has wrong calling convention! Must be _stdcall.", plugin.BaseName.c_str());
#endif
        break;
      case 1:   // stdcall
#ifndef AVSC_USE_STDCALL
            Env->ThrowError("Avisynth 2 C Plugin '%s' has wrong calling convention! Must be _cdecl.", plugin.BaseName.c_str());
#endif
        break;
      case 2:
        Env->ThrowError("Avisynth 2 C Plugin '%s' has corrupted the stack.", plugin.BaseName.c_str());
      }
#else
      s = AvisynthCPluginInit(pe);
#endif
//	    if (s == 0)
    //	    Env->ThrowError("Avisynth 2 C Plugin '%s' returned a NULL pointer.", plugin.BaseName.c_str());

      *result = AVSValue(s);
    }
    PluginInLoad = NULL;
  }

  return true;
}

/*
---------------------------------------------------------------------------------
---------------------------------------------------------------------------------
                                 LoadPlugin
---------------------------------------------------------------------------------
---------------------------------------------------------------------------------
*/

AVSValue LoadPlugin(AVSValue args, void* user_data, IScriptEnvironment* env) 
{
  IScriptEnvironment2 *env2 = static_cast<IScriptEnvironment2*>(env);

  bool success = true;
  for (int i = 0; i < args.ArraySize(); ++i)
  {
    AVSValue dummy;
    success &= env2->LoadPlugin(args[0][i].AsString(), true, &dummy);
  }

  return AVSValue(success);
}

extern const AVSFunction Plugin_functions[] = {
  {"LoadPlugin", BUILTIN_FUNC_PREFIX, "s+", LoadPlugin},
  {"LoadCPlugin", BUILTIN_FUNC_PREFIX, "s+", LoadPlugin },          // for compatibility with older scripts
  {"Load_Stdcall_Plugin", BUILTIN_FUNC_PREFIX, "s+", LoadPlugin },  // for compatibility with older scripts
  { 0 }
};