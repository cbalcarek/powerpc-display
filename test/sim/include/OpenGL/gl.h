typedef unsigned int GLenum, GLuint, GLbitfield; typedef int GLint, GLsizei; typedef unsigned char GLubyte;
#define GL_EXTENSIONS 1
#define GL_RENDERER 2
#define GL_VERSION 3
#define GL_PROJECTION 4
#define GL_MODELVIEW 5
#define GL_DEPTH_TEST 6
#define GL_DITHER 7
#define GL_LIGHTING 8
#define GL_TEXTURE_ENV 9
#define GL_TEXTURE_ENV_MODE 10
#define GL_REPLACE 11
#define GL_UNSIGNED_SHORT_1_5_5_5_REV 12
#define GL_UNSIGNED_INT_8_8_8_8_REV 13
#define GL_UNPACK_ALIGNMENT 14
#define GL_TEXTURE_MIN_FILTER 15
#define GL_TEXTURE_MAG_FILTER 16
#define GL_NEAREST 17
#define GL_TEXTURE_WRAP_S 18
#define GL_TEXTURE_WRAP_T 19
#define GL_CLAMP_TO_EDGE 20
#define GL_RGB5_A1 21
#define GL_RGBA 22
#define GL_BGRA 23
#define GL_TEXTURE 24
#define GL_BLEND 25
#define GL_QUADS 26
#define GL_ONE 27
#define GL_ONE_MINUS_SRC_ALPHA 28
#define GL_NO_ERROR 0
const GLubyte *glGetString(GLenum); void glViewport(GLint,GLint,GLsizei,GLsizei); void glMatrixMode(GLenum); void glLoadIdentity(void);
void glOrtho(double,double,double,double,double,double); void glDisable(GLenum); void glEnable(GLenum); void glTexEnvi(GLenum,GLenum,GLint);
void glClearColor(float,float,float,float); void glGenTextures(GLsizei,GLuint*); void glBindTexture(GLenum,GLuint);
void glTexParameteri(GLenum,GLenum,GLint); void glPixelStorei(GLenum,GLint); void glTexImage2D(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*);
void glTexSubImage2D(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,const void*); void glDeleteTextures(GLsizei,const GLuint*);
void glFinish(void); void glBegin(GLenum); void glEnd(void); void glTexCoord2i(GLint,GLint); void glVertex2i(GLint,GLint); void glBlendFunc(GLenum,GLenum);
GLenum glGetError(void); void glGetIntegerv(GLenum, GLint*);
#define GL_TRUE 1
#define GL_FALSE 0
#define GL_TEXTURE_2D 0x0DE1
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_UNPACK_ROW_LENGTH 0x0CF2
void glTexCoord2f(float, float);
