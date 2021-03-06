/*
	module.c: modular code loader

	copyright 1995-2015 by the mpg123 project - free software under the terms of the LGPL 2.1
	see COPYING and AUTHORS files in distribution or http://mpg123.org
	initially written by Nicholas J Humfrey
*/

/* Need snprintf(). */
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#include "config.h"
#include "intsym.h"
#include "stringlists.h"
#include "compat.h"
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>

#include "module.h"
#include "debug.h"

#ifndef USE_MODULES
#error This is a build without modules. Why am I here?
#endif

#if defined(HAVE_DLOPEN) && defined(HAVE_DLSYM) && defined(HAVE_DLCLOSE)
#  ifdef HAVE_DLFCN_H
#    include <dlfcn.h>
#  endif
#  define DLOPEN(a) dlopen((a), RTLD_NOW)
#  define DLSYM     dlsym
#  define DLCLOSE   dlclose
#else
#  if defined(HAVE_LOADLIBRARY) \
   && defined(HAVE_GETPROCADDRESS) \
   && defined(HAVE_FREELIBRARY)
#    ifdef HAVE_WINDOWS_H
#      include <windows.h>
#    endif
#    define DLOPEN    LoadLibrary
#    define DLSYM     GetProcAddress
#    define DLCLOSE   FreeLibrary
#  else
#    error No dynamic loading mechanism available.
#  endif
#endif

#define MODULE_SYMBOL_PREFIX 	"mpg123_"
#define MODULE_SYMBOL_SUFFIX 	"_module_info"

/* It's nasty to hardcode that here...
   also it does need hacking around libtool's hardcoded .la paths:
   When the .la file is in the same dir as .so file, you need libdir='.' in there. */
static const char* modulesearch[] =
{
	 "../lib/mpg123"
	,"plugins"
	,"libout123/modules/.libs"
	,"libout123/modules"
	,"../libout123/modules/.libs"
	,"../libout123/modules"
};

static char *get_the_cwd(int verbose); /* further down... */
static char *get_module_dir(int verbose, const char* bindir)
{
	DIR* dir = NULL;
	char *moddir = NULL;
	const char *defaultdir;
	/* First the environment override, then relative to bindir, then installation prefix. */
	defaultdir = getenv("MPG123_MODDIR");
	if(defaultdir)
	{
		if(verbose > 1)
			fprintf(stderr, "Trying module directory from environment: %s\n", defaultdir);
		dir = opendir(defaultdir);
		if(dir)
		{
			closedir(dir);
			moddir = compat_strdup(defaultdir);
		}
	}
	else if(bindir) /* Search relative to binary. */
	{
		size_t i;
		size_t bl = strlen(bindir);
		for(i=0; i<sizeof(modulesearch)/sizeof(char*); ++i)
		{
			const char *testpath = modulesearch[i];
			size_t l;

			l = bl + strlen(testpath) + 1;
			moddir = malloc(l+1);
			if(moddir)
			{
				snprintf(moddir, l+1, "%s/%s", bindir, testpath);
				moddir[l] = 0;
				if(verbose > 1)
					fprintf(stderr, "Looking for module dir: %s\n", moddir);
				dir = opendir(moddir);
				if(dir != NULL)
				{
					closedir(dir);
					break; /* found it! */
				}
				else{ free(moddir); moddir=NULL; }
			}
		}
	}
	if(!moddir) /* Resort to installation prefix. */
	{
		defaultdir=PKGLIBDIR;
		dir = opendir(defaultdir);
		if(dir)
		{
			closedir(dir);
			if(verbose > 1)
				fprintf(stderr, "Using default module dir: %s\n", defaultdir);
			moddir = compat_strdup(defaultdir);
		}
	}

	if(verbose > 1)
		fprintf(stderr, "Module dir: %s\n", moddir != NULL ? moddir : "<nil>");
	return moddir;
}

/* Open a module in current directory. */
mpg123_module_t* open_module_here(const char* type, const char* name, int verbose)
{
	void* handle = NULL;
	mpg123_module_t *module = NULL;
	char* module_path = NULL;
	size_t module_path_len = 0;
	char* module_symbol = NULL;
	size_t module_symbol_len = 0;

	/* Work out the path of the module to open */
	/* Note that we need to open ./file, not just file! */
	module_path_len = 2 + strlen(type) + 1 + strlen(name) + strlen(LT_MODULE_EXT) + 1;
	module_path = malloc( module_path_len );
	if (module_path == NULL) {
		if(verbose > -1)
			error1( "Failed to allocate memory for module name: %s", strerror(errno) );
		return NULL;
	}
	snprintf( module_path, module_path_len, "./%s_%s%s", type, name, LT_MODULE_EXT );
	/* Display the path of the module created */
	if(verbose > 1)
		fprintf(stderr, "Module path: %s\n", module_path );

	/* Open the module */
	handle = DLOPEN(module_path);
	free(module_path);
	if (handle==NULL)
	{
		error1("Failed to open module %s.", name);
		return NULL;
	}
	
	/* Work out the symbol name */
	module_symbol_len = strlen( MODULE_SYMBOL_PREFIX ) +
						strlen( type )  +
						strlen( MODULE_SYMBOL_SUFFIX ) + 1;
	module_symbol = malloc(module_symbol_len);
	if (module_symbol == NULL) {
		if(verbose > -1)
			error1( "Failed to allocate memory for module symbol: %s", strerror(errno) );
		return NULL;
	}
	snprintf( module_symbol, module_symbol_len, "%s%s%s", MODULE_SYMBOL_PREFIX, type, MODULE_SYMBOL_SUFFIX );
	debug1( "Module symbol: %s", module_symbol );
	
	/* Get the information structure from the module */
	module = (mpg123_module_t*)DLSYM(handle, module_symbol );
	free( module_symbol );
	if (module==NULL) {
		error("Failed to get module symbol.");
		return NULL;
	}
	
	/* Check the API version */
	if (MPG123_MODULE_API_VERSION != module->api_version)
	{
		error2( "API version of module does not match (got %i, expected %i).", module->api_version, MPG123_MODULE_API_VERSION);
		DLCLOSE(handle);
		return NULL;
	}

	/* Store handle in the data structure */
	module->handle = handle;
	return module;
}


/* Open a module, including directory search. */
mpg123_module_t* open_module( const char* type, const char* name, int verbose
,	const char* bindir )
{
	mpg123_module_t *module = NULL;
	char *workdir = NULL;
	char *moddir  = NULL;

	workdir = get_the_cwd(verbose);
	moddir  = get_module_dir(verbose, bindir);
	if(workdir == NULL || moddir == NULL)
	{
		if(verbose > -1)
		{
			error("Failure getting workdir or moddir! (Perhaps set MPG123_MODDIR?)");
			if(workdir == NULL)
				fprintf(stderr, "Hint: I need to know the current working directory to be able to come back after hunting modules. I will not leave because I do not know where I am.\n");
		}
		if(workdir != NULL) free(workdir);
		if(moddir  != NULL) free(moddir);
		return NULL;
	}

	if(chdir(moddir) == 0)
		module = open_module_here(type, name, verbose);
	else if(verbose > -1)
		error2("Failed to enter module directory %s: %s", moddir, strerror(errno));

	chdir(workdir);
	free(moddir);
	free(workdir);
	return module;
}

void close_module( mpg123_module_t* module, int verbose )
{
	int err = DLCLOSE(module->handle);
	
	if(err && verbose > -1)
		error("Failed to close module.");
}

#define PATH_STEP 50
static char *get_the_cwd(int verbose)
{
	size_t bs = PATH_STEP;
	char *buf = malloc(bs);
	errno = 0;
	while((buf != NULL) && getcwd(buf, bs) == NULL)
	{
		char *buf2;
		if(errno != ERANGE)
		{
			if(verbose > -1)
				error1("getcwd returned unexpected error: %s", strerror(errno));
			free(buf);
			return NULL;
		}
		buf2 = realloc(buf, bs+=PATH_STEP);
		if(buf2 == NULL){ free(buf); buf = NULL; }
		else debug1("pwd: increased buffer to %lu", (unsigned long)bs);

		buf = buf2;
	}
	return buf;
}

int list_modules( const char *type, char ***names, char ***descr, int verbose
,	const char* bindir )
{
	DIR* dir = NULL;
	struct dirent *dp = NULL;
	char *moddir  = NULL;
	int count = 0;

	debug1("verbose:%i", verbose);

	*names = NULL;
	*descr = NULL;

	moddir = get_module_dir(verbose, bindir);
	if(moddir == NULL)
	{
		if(verbose > -1)
			error("Failure getting module directory! (Perhaps set MPG123_MODDIR?)");
		return -1;
	}
	debug1("module dir: %s", moddir);
	/* Open the module directory */
	dir = moddir != NULL ? opendir(moddir) : NULL;
	if (dir==NULL) {
		if(verbose > -1)
			error2("Failed to open the module directory (%s): %s\n"
			,	PKGLIBDIR, strerror(errno));
		free(moddir);
		return -1;
	}

	if(chdir(moddir) != 0)
	{
		if(verbose > -1)
			error2("Failed to enter module directory (%s): %s\n"
			,	PKGLIBDIR, strerror(errno));
		closedir(dir);
		free(moddir);
		return -1;
	}
	free(moddir);

	while( count >= 0 && (dp = readdir(dir)) != NULL )
	{
		/* Copy of file name to be cut into pieces. */
		char *module_typename = NULL;
		/* Pointers to the pieces. */
		char *module_name = NULL;
		char *module_type = NULL;
		char *uscore_pos = NULL;
		mpg123_module_t *module = NULL;
		char* ext;
		struct stat fst;
		size_t name_len;

		/* Various checks as loop shortcuts, avoiding too much nesting. */
		debug1("checking entry: %s", dp->d_name);

		if(stat(dp->d_name, &fst) != 0)
			continue;
		if(!S_ISREG(fst.st_mode)) /* Allow links? */
			continue;

		name_len = strlen(dp->d_name);
		if(name_len < strlen(LT_MODULE_EXT))
			continue;
		ext = dp->d_name
		+	name_len
		-	strlen(LT_MODULE_EXT);
		if(strcmp(ext, LT_MODULE_EXT))
			continue;
		debug("has suffix");

		/* Extract the module type and name */
		if(!(module_typename=compat_strdup(dp->d_name)))
			continue;
		uscore_pos = strchr( module_typename, '_' );
		if(   uscore_pos==NULL
		  || (uscore_pos>=module_typename+name_len+1) )
		{
			free(module_typename);
			continue;
		}
		*uscore_pos = '\0';
		module_type = module_typename;
		module_name = uscore_pos+1;
		/* Only list modules of desired type. */
		if(strcmp(type, module_type))
		{
			debug("wrong type");
			free(module_typename);
			continue;
		}
		debug("has type");

		/* Extract the short name of the module */
		name_len -= uscore_pos - module_typename + 1;
		if(name_len <= strlen(LT_MODULE_EXT))
		{
			debug("name too short");
			free(module_typename);
			continue;
		}
		name_len -= strlen(LT_MODULE_EXT);
		module_name[name_len] = '\0';

		debug("opening module");
		/* Open the module
		   Yes, this re-builds the file name we chopped to pieces just now. */
		if((module=open_module_here(module_type, module_name, verbose)))
		{
			if( stringlists_add( names, descr
			,	module->name, module->description, &count) )
				if(verbose > -1)
					error("OOM");
			/* Close the module again */
			close_module(module, verbose);
		}
		free(module_typename);
	}
	closedir(dir);
	return count;
}


