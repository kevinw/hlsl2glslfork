#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <string>
#include <vector>
#include <fstream>
#include <time.h>
#include <assert.h>

#define USE_REAL_OPENGL_TO_CHECK 0

#ifdef _MSC_VER
#include <windows.h>
#include <gl/GL.h>
#include <cstdarg>

extern "C" {
typedef char GLchar;		/* native character */
#define GL_VERTEX_SHADER              0x8B31
#define GL_FRAGMENT_SHADER            0x8B30
#define GL_COMPILE_STATUS      0x8B81
typedef void (WINAPI * PFNGLDELETESHADERPROC) (GLuint obj);
typedef GLuint (WINAPI * PFNGLCREATESHADERPROC) (GLenum shaderType);
typedef void (WINAPI * PFNGLSHADERSOURCEPROC) (GLuint shaderObj, GLsizei count, const GLchar* *string, const GLint *length);
typedef void (WINAPI * PFNGLCOMPILESHADERPROC) (GLuint shaderObj);
typedef void (WINAPI * PFNGLGETSHADERINFOLOGPROC) (GLuint obj, GLsizei maxLength, GLsizei *length, GLchar *infoLog);
typedef void (WINAPI * PFNGLGETSHADERIVPROC) (GLuint obj, GLenum pname, GLint *params);
static PFNGLDELETESHADERPROC glDeleteShader;
static PFNGLCREATESHADERPROC glCreateShader;
static PFNGLSHADERSOURCEPROC glShaderSource;
static PFNGLCOMPILESHADERPROC glCompileShader;
static PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog;
static PFNGLGETSHADERIVPROC glGetShaderiv;
}

// forks stdio to debug console so when you hit a breakpoint you can more easily see what test is running:
static void logf(const char* format, ...)
{
    va_list args = NULL;
    va_start(args, format);
    vprintf(format, args);
    va_end(args); //lint !e1924: C-style cast
    
    char buffer[4096];
    const size_t bufferSize = sizeof(buffer) - 1;

    args = NULL;
    va_start(args, format);
    int rc = _vsnprintf(buffer, bufferSize, format, args);
    va_end(args); //lint !e1924: C-style cast
    
    size_t outputLen = (rc <= 0) ? 0 : static_cast<size_t>(rc);
    buffer[outputLen] = '\0';

    OutputDebugStringA(buffer);
}

#define printf logf
#define snprintf _snprintf


#elif defined(__APPLE__)

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/CGLTypes.h>
#include <dirent.h>
#include <unistd.h>

#else

#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <GL/glew.h>
#include <GL/glut.h>

#endif

#if defined(__APPLE__)
#include "hlsl2glsl.h"
#define DLLEXPORT
#else
#include "../../include/hlsl2glsl.h"
#define DLLEXPORT __declspec(dllexport)
#endif
static void replace_string (std::string& target, const std::string& search, const std::string& replace, size_t startPos);

typedef std::vector<std::string> StringVector;

static bool ReadStringFromFile (const char* pathName, std::string& output)
{
#	ifdef _MSC_VER
	wchar_t widePath[MAX_PATH];
	int res = ::MultiByteToWideChar (CP_UTF8, 0, pathName, -1, widePath, MAX_PATH);
	if (res == 0)
		widePath[0] = 0;
	FILE* file = _wfopen(widePath, L"rb");
#	else // ifdef _MSC_VER
	FILE* file = fopen(pathName, "rb");
#	endif // !ifdef _MSC_VER

	if (file == NULL)
		return false;
	
	fseek(file, 0, SEEK_END);
	long length = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (length < 0)
	{
		fclose( file );
		return false;
	}
	
	output.resize(length);
	size_t readLength = fread(&*output.begin(), 1, length, file);
	
	fclose(file);
	
	if (readLength != length)
	{
		output.clear();
		return false;
	}

	replace_string(output, "\r\n", "\n", 0);
	
	return true;
}

static void replace_string (std::string& target, const std::string& search, const std::string& replace, size_t startPos)
{
	if (search.empty())
		return;
	
	std::string::size_type p = startPos;
	while ((p = target.find (search, p)) != std::string::npos)
	{
		target.replace (p, search.size (), replace);
		p += replace.size ();
	}
}


enum TestRun {
	VERTEX,
	FRAGMENT,
	BOTH,
	VERTEX_120,
	FRAGMENT_120,
	VERTEX_FAILURES,
	FRAGMENT_FAILURES,
	NUM_RUN_TYPES
};


static std::string GetCompiledShaderText(ShHandle parser)
{
	std::string txt = Hlsl2Glsl_GetShader (parser);
	
	int count = Hlsl2Glsl_GetUniformCount (parser);
    
    if (count > 0)
	{
		const ShUniformInfo* uni = Hlsl2Glsl_GetUniformInfo(parser);
		txt += "\n// uniforms:\n";
		for (int i = 0; i < count; ++i)
		{
			char buf[1000];
			snprintf(buf,1000,"// %s:%s type %d arrsize %d", uni[i].name, uni[i].semantic?uni[i].semantic:"<none>", uni[i].type, uni[i].arraySize);
			txt += buf;

			if (uni[i].registerSpec)
			{
				txt += " register ";
				txt += uni[i].registerSpec;
			}

			txt += "\n";
		}
	}
	
	return txt;
}


struct IncludeContext {
	std::string currentFolder;
  EShLanguage shaderType;
};

typedef const char* (__stdcall * IncludeCallback)(const char* filename, EShLanguage shaderType);

IncludeCallback customIncludeCallback;

static bool C_DECL IncludeOpenCallback(bool isSystem, const char* fname, const char* parentfname, const char* parent, std::string& output, void* d) {
    const IncludeContext* data = reinterpret_cast<IncludeContext*>(d);
	
    std::string pathName = data->currentFolder + "/" + fname;

    if (customIncludeCallback) {
        const char* bytes = customIncludeCallback(pathName.c_str(), data->shaderType);
        if (bytes)
            output = bytes;
        return bytes != nullptr;
    } else {
        return ReadStringFromFile(pathName.c_str(), output);
    }
}

static bool didInitialize = false;
extern "C" {

void SetIncludeCallback(IncludeCallback includeCallback) {
  customIncludeCallback = includeCallback;
}

DLLEXPORT bool Parse(EShLanguage shaderType,
	const char* sourceStr,
	const char* includePath,
	ETargetVersion version,
	char* output,
	int outputMaxCount)
{
	if (!didInitialize) {
		didInitialize = true;
		Hlsl2Glsl_Initialize();
	}

	assert(version != ETargetVersionCount);

	ShHandle parser = Hlsl2Glsl_ConstructCompiler(shaderType);

	IncludeContext includeCtx;
	includeCtx.currentFolder = std::string(includePath);
	includeCtx.shaderType = shaderType;

	Hlsl2Glsl_ParseCallbacks includeCB;
	includeCB.includeOpenCallback = IncludeOpenCallback;
	includeCB.includeCloseCallback = NULL;
	includeCB.data = &includeCtx;

	unsigned int options = 0;
	bool parseOk = 1 == Hlsl2Glsl_Parse(parser, sourceStr, version, &includeCB, options);	
	const char* infoLog = Hlsl2Glsl_GetInfoLog(parser);
	if (infoLog)
		strncpy(output, infoLog, outputMaxCount);
	Hlsl2Glsl_DestructCompiler(parser);
	return parseOk;
}

DLLEXPORT bool TranslateShader (EShLanguage shaderType,
                      const char* sourceStr,
                      const char* includePath,
                      const char* entryPoint,
                      ETargetVersion version,
                      unsigned options,
                      char* output,
                      int outputMaxCount)
{
    if (!didInitialize) {
        didInitialize = true;
        Hlsl2Glsl_Initialize ();
    }
    
    assert(version != ETargetVersionCount);
    
    ShHandle parser = Hlsl2Glsl_ConstructCompiler (shaderType);
    
    IncludeContext includeCtx;
    includeCtx.currentFolder = std::string(includePath);
    includeCtx.shaderType = shaderType;

    Hlsl2Glsl_ParseCallbacks includeCB;
    includeCB.includeOpenCallback = IncludeOpenCallback;
    includeCB.includeCloseCallback = NULL;
    includeCB.data = &includeCtx;
    
    int parseOk = Hlsl2Glsl_Parse (parser, sourceStr, version, &includeCB, options);
    const char* infoLog = Hlsl2Glsl_GetInfoLog( parser );
    bool res = true;
    
    if (parseOk) {
        static EAttribSemantic kAttribSemantic[] = {
            EAttrSemTangent,
        };
        static const char* kAttribString[] = {
            "TANGENT",
        };
        Hlsl2Glsl_SetUserAttributeNames (parser, kAttribSemantic, kAttribString, 1);
        
        int translateOk = Hlsl2Glsl_Translate (parser, entryPoint, version, options);
        const char* infoLog = Hlsl2Glsl_GetInfoLog( parser );
        if (translateOk) {
            std::string text = GetCompiledShaderText(parser);
            
            if ((size_t)outputMaxCount < text.size()) {
                res = false;
                strncpy(output, "not enough space in output buffer for shader", outputMaxCount);
            } else {
            
                strncpy(output, text.c_str(), outputMaxCount);
                
                for (size_t i = 0, n = text.size(); i != n; ++i) {
                    char c = text[i];
                    
                    if (!isascii(c)) {
                        printf ("  contains non-ascii '%c' (0x%02X)\n", c, c);
                        res = false;
                    }
                }
            }
        } else {
            printf ("  translate error: %s\n", infoLog);
            strncpy(output, infoLog, outputMaxCount);

            res = false;
        }
    } else {
        printf ("  parse error: %s\n", infoLog);
        strncpy(output, infoLog, outputMaxCount);

        res = false;
    }
    
    if (res) {
        std::fstream debugOut;
        debugOut.open("/Users/kevin/Desktop/shader_error_in.hlsl");
        debugOut << sourceStr;
        debugOut.close();
    }
    
    Hlsl2Glsl_DestructCompiler (parser);
    
    return res;
}


}
