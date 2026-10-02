#include "iggy.h"
// rrCore's Windows breakpoint assumes the MSVC inline-assembly syntax.
#undef RR_BREAK
#define RR_BREAK() __builtin_trap()
// The archived WGL extension table omits this entry used by its ihud shader.
#define glUniform1f(location,value) ((PFNGLUNIFORM1FPROC)wglGetProcAddress("glUniform1f"))(location,value)
