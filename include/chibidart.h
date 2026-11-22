#ifndef CHIBIDART_H
#define CHIBIDART_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define CD_OK 0
#define CD_ERR_COMPILE 1
#define CD_ERR_RUNTIME 2

typedef struct cd_State cd_State;
typedef int (*cd_CFunction)(cd_State* cd);

cd_State* cd_open(void);
void cd_close(cd_State* cd);

int cd_dostring(cd_State* cd, const char* source);
int cd_dofile(cd_State* cd, const char* path);

void cd_pushnil(cd_State* cd);
void cd_pushbool(cd_State* cd, bool b);
void cd_pushint(cd_State* cd, int64_t val);
void cd_pushfloat(cd_State* cd, double val);
void cd_pushstring(cd_State* cd, const char* str);
void cd_pushlstring(cd_State* cd, const char* str, size_t len);

bool cd_isnil(cd_State* cd, int idx);
bool cd_isbool(cd_State* cd, int idx);
bool cd_isint(cd_State* cd, int idx);
bool cd_isfloat(cd_State* cd, int idx);
bool cd_isstring(cd_State* cd, int idx);

bool cd_tobool(cd_State* cd, int idx);
int64_t cd_toint(cd_State* cd, int idx);
double cd_tofloat(cd_State* cd, int idx);
const char* cd_tostring(cd_State* cd, int idx);

int cd_gettop(cd_State* cd);
void cd_settop(cd_State* cd, int idx);
void cd_pop(cd_State* cd, int n);

void cd_getglobal(cd_State* cd, const char* name);
void cd_setglobal(cd_State* cd, const char* name);
void cd_register(cd_State* cd, const char* name, cd_CFunction fn);

int cd_pcall(cd_State* cd, int nargs, int nresults);

#endif
