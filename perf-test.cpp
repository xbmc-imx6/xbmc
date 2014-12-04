//#define LINUX
//#define EGL_API_FB
#define GL_GLEXT_PROTOTYPES

#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/fb.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <cores/dvdplayer/DVDCodecs/Video/DVDVideoCodecIMX.h>
#include <cores/dvdplayer/DVDCodecs/DVDCodecs.h>
#include <cores/dvdplayer/DVDClock.h>
#include <xbmc/threads/Thread.h>
#include <xbmc/settings/MediaSettings.h>
#include <xbmc/utils/log.h>

#include <iostream>
#include <iomanip>
#include <vector>

using namespace std;

EGLDisplay display;
EGLContext context;
EGLSurface surface;

#define CheckError(func) do { EGLint result = eglGetError(); if(result != EGL_SUCCESS) { printf("EGL error in %s: %x\n", func, result); return 1;} /*else printf("%s OK\n", func);*/ } while (0)

EGLint const attribute_list[] = {
		EGL_RED_SIZE,        8,
		EGL_GREEN_SIZE,      8,
		EGL_BLUE_SIZE,       8,
		EGL_ALPHA_SIZE,      0,
		EGL_DEPTH_SIZE,      0,
		EGL_STENCIL_SIZE,    0,
		EGL_SAMPLE_BUFFERS,  0,
		EGL_SAMPLES,         0,
		EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
};

EGLint ctxattr[] = {
	EGL_CONTEXT_CLIENT_VERSION, 2,
	EGL_NONE
};

struct fb_var_screeninfo screeninfo;

int initFB() {
	int fd;

	fd = open("/dev/fb0",O_RDWR);
	if (fd < 0)
	{
		printf("Could not open fb0\n");
		return 1;
	}

	if ( ioctl(fd, FBIOGET_VSCREENINFO, &screeninfo) != 0 ) {
		printf("Query screeninfo failed\n");
		return 1;
	}

	// Unblank the fbs
	if (ioctl(fd, FBIOBLANK, 0) < 0)
	{
		printf("Error while unblanking\n");
		return 1;
	}

	close(fd);
	return 0;
}


int initEGL() {
	EGLNativeDisplayType native_display;
	EGLConfig config;
	NativeWindowType native_window;
	EGLint num_config;

	/* get an EGL display connection */
	native_display = fbGetDisplayByIndex(0);
	CheckError("fbGetDisplayByIndex");

	/* create a native window */
	native_window = fbCreateWindow(native_display, 0, 0, screeninfo.xres, screeninfo.yres);
	//native_window = open("/dev/fb0", O_RDWR);
	CheckError("fbCreateWindow");

	display = eglGetDisplay(native_display);
	CheckError("eglGetDisplay");

	/* initialize the EGL display connection */
	eglInitialize(display, NULL, NULL);
	CheckError("eglInitialize");

	/* get an appropriate EGL frame buffer configuration */
	eglChooseConfig(display, attribute_list, NULL, 0, &num_config);
	CheckError("eglChooseConfig");

	if ( num_config == 0 ) {
		printf("No appropriate configs\n");
		return 1;
	}

	eglChooseConfig(display, attribute_list, &config, 1, &num_config);
	CheckError("eglChooseConfig");

	/* create an EGL window surface */
	surface = eglCreateWindowSurface(display, config, native_window, NULL);
	CheckError("eglCreateWindowSurface");

	/* create an EGL rendering context */
	context = eglCreateContext(display, config, NULL, ctxattr);
	CheckError("eglCreateContext");

	/* connect the context to the surface */
	eglMakeCurrent(display, surface, surface, context);
	CheckError("eglMakeCurrent");

	return 0;
}


int destroyEGL() {
	eglDestroyContext(display, context);
	CheckError("eglDestroyContext");

	eglDestroySurface(display, surface);
	CheckError("eglDestroySurface");

	eglTerminate(display);
	CheckError("eglTerminate");

	return 0;
}


GLuint loadShader(const char *shader_source, GLenum type) {
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &shader_source, NULL);
	glCompileShader(shader);
	return shader;
}


const char vertex_src [] =
"                                        \
   attribute vec4 position;              \
   attribute vec2 tc;                    \
   varying vec2 frag_tc;                 \
                                         \
   void main()                           \
   {                                     \
      frag_tc = tc;                      \
      gl_Position = position;            \
   }                                     \
";


const char fragment_src [] =
"                                                      \
   uniform sampler2D tex;                              \
   varying vec2 frag_tc;                               \
                                                       \
   void  main()                                        \
   {                                                   \
      gl_FragColor = texture2D(tex, frag_tc);          \
   }                                                   \
";

GLint position_loc, texture_loc;
GLfloat vertices[] = {
	-1.0,  1.0,  0.0,
	 1.0,  1.0,  0.0,
	-1.0, -1.0,  0.0,
	 1.0, -1.0,  0.0
};

GLfloat texCoords[] = {
	0.0, 0.0,
	1.0, 0.0,
	0.0, 1.0,
	1.0, 1.0
};

class Queue {
	public:
		Queue() {}
		~Queue() {
			for ( size_t i = 0; i < m_buffer.size(); ++i )
				SAFE_RELEASE(m_buffer[i].IMXBuffer);
		}

		void SetCapacity(int input) {
			m_buffer.resize(input);
			for ( size_t i = 0; i < m_buffer.size(); ++i )
				m_buffer[i].IMXBuffer = NULL;

			// Reset ring buffer
			m_beginInput = m_endInput = m_bufferedInput = 0;
			m_bClosed = false;
		}

		// Returns false if Queue is closed
		bool Push(const DVDVideoPicture &p) {
			CSingleLock lk(m_monitor);

			// If the input queue is full, wait for a free slot
			while ( (m_bufferedInput == m_buffer.size()) && !m_bClosed )
				m_inputNotFull.wait(lk);

			if ( m_bClosed ) {
				m_inputNotEmpty.notifyAll();
				return false;
			}

			// Store the value
			m_buffer[m_endInput] = p;
			// Lock the buffer
			p.IMXBuffer->Lock();
			m_endInput = (m_endInput+1) % m_buffer.size();
			++m_bufferedInput;
			m_inputNotEmpty.notifyAll();

			return true;
		}

		// Returns false if Queue is closed otherwise blocks
		// until output is available
		bool Pop(DVDVideoPicture &p) {
			CSingleLock lk(m_monitor);

			while ( m_bufferedInput == 0 && !m_bClosed ) {
				m_inputNotEmpty.wait(lk);
			}

			if ( m_bClosed )
				return false;

			p = m_buffer[m_beginInput];
			m_buffer[m_beginInput].IMXBuffer = NULL;
			m_beginInput = (m_beginInput+1) % m_buffer.size();
			--m_bufferedInput;
			m_inputNotFull.notifyAll();

			return true;
		}

		void Close() {
			if ( m_bClosed ) return;
			m_bClosed = true;
			m_inputNotFull.notifyAll();
			m_inputNotEmpty.notifyAll();
		}

	private:
		typedef vector<DVDVideoPicture> PictureBuffer;

		PictureBuffer                   m_buffer;
		volatile int                    m_beginInput, m_endInput;
		volatile size_t                 m_bufferedInput;
		XbmcThreads::ConditionVariable  m_inputNotEmpty;
		XbmcThreads::ConditionVariable  m_inputNotFull;
		bool                            m_bClosed;

		mutable CCriticalSection        m_monitor;
};


class BufferIterator {
	public:
		BufferIterator(CDVDVideoCodec *codec, const char *fn) {
			_codec = codec;
			_fp = fopen(fn, "rb");
			if ( _fp != NULL ) {
				CDVDStreamInfo hints;
				CDVDCodecOptions options;
				bool ok = true;
				ok = (fread(&hints.software, sizeof(hints.software), 1, _fp) == 1) && ok;
				ok = (fread(&hints.codec, sizeof(hints.codec), 1, _fp) == 1) && ok;
				ok = (fread(&hints.profile, sizeof(hints.profile), 1, _fp) == 1) && ok;
				ok = (fread(&hints.codec_tag, sizeof(hints.codec_tag), 1, _fp) == 1) && ok;
				ok = (fread(&hints.extrasize, sizeof(hints.extrasize), 1, _fp) == 1) && ok;
				if ( !ok ) {
					fclose(_fp);
					_fp = NULL;
					cerr << "Invalid header" << endl;
				}
				else {
					cerr << "Reading extradata with " << hints.extrasize << " bytes" << endl;
					hints.extradata = hints.extrasize > 0?malloc(hints.extrasize):NULL;
					if ( (hints.extradata != NULL) && (fread(hints.extradata, 1, hints.extrasize, _fp) != hints.extrasize) ) {
						fclose(_fp);
						_fp = NULL;
						cerr << "Could not read extradata" << endl;
					}
					else if ( !_codec->Open(hints, options) ) {
						cerr << "Could not open codec" << endl;
						fclose(_fp);
						_fp = NULL;
					}
				}
			}

			memset(&_pic, 0, sizeof(DVDVideoPicture));
			_state = VC_BUFFER;
		}

		~BufferIterator() {
			if ( _fp != NULL )
				fclose(_fp);
		}

		DVDVideoPicture *next() {
			while ( true ) {
				if ( _state & VC_BUFFER ) {
					double dts, pts;
					int size;

					if ( _fp == NULL ) {
						cerr << "Invalid file handle" << endl;
						return NULL;
					}

					bool ok = true;
					ok = (fread(&dts, sizeof(dts), 1, _fp) == 1) && ok;
					ok = (fread(&pts, sizeof(pts), 1, _fp) == 1) && ok;
					ok = (fread(&size, sizeof(size), 1, _fp) == 1) && ok;

					if ( !ok || (size <= 0) ) {
						cerr << "Wrong chunk header" << endl;
						return NULL;
					}

					if ( _buf.size() < size ) _buf.reserve(size);
					_buf.resize(size);

					if ( fread(&_buf[0], sizeof(BYTE), (int)_buf.size(), _fp) != (int)_buf.size() ) {
						cerr << "Failed to read chunk" << endl;
						return NULL;
					}

					// Call decode
					_state = _codec->Decode(&_buf[0], (int)_buf.size(), dts, pts);
				}
				else if ( !_state )
					_state = _codec->Decode(NULL, 0, DVD_NOPTS_VALUE, DVD_NOPTS_VALUE);

				if ( _state & VC_ERROR )
					return NULL;

				if ( _state & VC_PICTURE ) {
					_state &= ~VC_PICTURE;
					_codec->ClearPicture(&_pic);
					if ( _codec->GetPicture(&_pic) )
						return &_pic;
					else {
						CLog::Log(LOGWARNING, "Decoder::GetPicture failed: reset");
						_codec->Reset();
					}
				}
			}

			return NULL;
		}

	private:
		CDVDVideoCodec  *_codec;
		DVDVideoPicture  _pic;
		FILE            *_fp;
		vector<BYTE>     _buf;
		int              _state;
};


class OutputBase : private CThread {
	protected:
		OutputBase(Queue *q) : CThread("Output"), m_queue(q) {}

	public:
		void Start() {
			Create();
		}

		bool Wait() {
			return WaitForThreadExit(10000);
		}


	protected:
		virtual bool Init() = 0;

		virtual bool Run() {
			DVDVideoPicture p;

			while ( m_queue->Pop(p) ) {
				if ( !Ouput(p) )
					return false;
			}

			return true;
		}

		virtual void Done() = 0;

		// Return false to break the run loop
		virtual bool Ouput(DVDVideoPicture &) = 0;

	private:
		virtual void Process() {
			if ( !Init() ) return;
			Run();
			Done();
		}

	private:
		Queue *m_queue;
};


class Stats : public OutputBase {
	public:
		Stats(Queue *q) : OutputBase(q) {}

		virtual bool Init() {
			m_frameCount = 0;
			m_before = XbmcThreads::SystemClockMillis();
			return true;
		}

		virtual void Done() {
			m_after = XbmcThreads::SystemClockMillis();

			if ( m_frameCount > 0 ) {
				cerr << "Decoding of " << m_frameCount << " frames took " << (m_after-m_before) << " ms" << endl;
				cerr << "Average frame decoding time: " << ((m_after-m_before)/m_frameCount) << " ms" << endl;
			}
			else
				cerr << "No frames rendered" << endl;
		}

		virtual bool Ouput(DVDVideoPicture &p) {
			SAFE_RELEASE(p.IMXBuffer);
			//cerr << m_frameCount << endl;
			++m_frameCount;
			return true;
		}

	private:
		int                m_frameCount;
		unsigned long long m_before, m_after;
};


class EGL : public Stats {
	public:
		EGL(Queue *q) : Stats(q) {}

		virtual bool Init() {
			Stats::Init();

			if ( initFB() ) {
				cerr << "FB init failed" << endl;
				return false;
			}

			if ( initEGL() ) {
				cerr << "EGL init failed" << endl;
				return false;
			}

			m_lastBuffer = NULL;
			GLuint vertexShader = loadShader(vertex_src, GL_VERTEX_SHADER);
			GLuint fragmentShader = loadShader(fragment_src, GL_FRAGMENT_SHADER);
			GLuint shaderProg = glCreateProgram();
			glAttachShader(shaderProg, vertexShader);
			glAttachShader(shaderProg, fragmentShader);

			glLinkProgram(shaderProg);
			glUseProgram(shaderProg);

			position_loc = glGetAttribLocation(shaderProg, "position");
			texture_loc  = glGetAttribLocation(shaderProg, "tc");

			glGenTextures(1, &m_textureID);

			glClearColor(0.0,0.0,0.0,1.0);
			glViewport(0, 0, screeninfo.xres, screeninfo.yres);
			glEnable(GL_TEXTURE_2D);

			return true;
		}

		virtual void Done() {
			Stats::Done();
			destroyEGL();
			SAFE_RELEASE(m_lastBuffer);
		}

		virtual bool Ouput(DVDVideoPicture &p) {
			SAFE_RELEASE(m_lastBuffer);
			m_lastBuffer = p.IMXBuffer;
			m_lastBuffer->Lock();

			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, m_textureID);

			GLuint physical = ~0u;
			GLvoid *virt = (GLvoid*)p.IMXBuffer->pVirtAddr;
			GLenum format = GL_INVALID_ENUM;

			switch ( p.IMXBuffer->iFormat ) {
				case 0:
					format = GL_VIV_I420;
					break;
				case 1:
					format = GL_VIV_NV12;
					break;
				case 2:
					format = GL_RGB565;
					break;
				case 3:
					format = GL_RGBA;
					break;
				default:
					cerr << "Unsupported buffer format" << endl;
					break;
			}

			if ( format != GL_INVALID_ENUM ) {
				glTexDirectVIVMap(GL_TEXTURE_2D, p.IMXBuffer->iWidth, p.IMXBuffer->iHeight, format,
				                  &virt, &physical);

				glTexDirectInvalidateVIV(GL_TEXTURE_2D);
				glVertexAttribPointer(position_loc, 3, GL_FLOAT, false, 0, vertices);
				glVertexAttribPointer(texture_loc, 2, GL_FLOAT, false, 0, texCoords);
				glEnableVertexAttribArray(position_loc);
				glEnableVertexAttribArray(texture_loc);
				glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			}

			eglSwapBuffers(display, surface);

			return Stats::Ouput(p);
		}

	private:
		GLuint                   m_textureID;
		CDVDVideoCodecIMXBuffer *m_lastBuffer;
};


int main (int argc, char *argv[]) {
	if ( argc < 2 ) {
		printf("Need stream dump file\n");
		return 1;
	}

	bool progressiveTest = false;
	bool deinterlacedTest = false;
	bool progressiveRenderTest = false;
	bool deinterlacedRenderTest = false;

	CLog::Init("./");
	CLog::SetLogLevel(LOG_LEVEL_DEBUG);

	for ( int i = 1; i < argc; ++i ) {
		const char *parg = argv[i-1];
		if ( !strcmp(parg, "-vscale") ) {
			double scale = atof(argv[i]);
			cerr << "Set quad size to " << scale << endl;
			for ( int v = 0; v < 4; ++v ) {
				vertices[v*3] *= scale;
				vertices[v*3+1] *= scale;
			}
		}
		if ( !strcmp(parg, "-tscale") ) {
			double scale = atof(argv[i]);
			cerr << "Set texture map area to " << scale << endl;
			for ( int t = 0; t < 4; ++t ) {
				texCoords[t*2] *= scale;
				texCoords[t*2+1] *= scale;
			}
		}
		else if ( !strcmp(argv[i], "-p") )
			progressiveTest = true;
		else if ( !strcmp(argv[i], "-d") )
			deinterlacedTest = true;
		else if ( !strcmp(argv[i], "-pr") )
			progressiveRenderTest = true;
		else if ( !strcmp(argv[i], "-dr") )
			deinterlacedRenderTest = true;
	}

	if ( progressiveTest ) {
		cerr << "Set deinterlacing to OFF" << endl;
		CMediaSettings::Get().GetCurrentVideoSettings().m_DeinterlaceMode = VS_DEINTERLACEMODE_OFF;

		CDVDVideoCodecIMX codec;
		Queue queue;
		BufferIterator it(&codec, argv[1]);
		DVDVideoPicture *pic;
		Stats stats(&queue);

		queue.SetCapacity(3);
		stats.Start();

		while ( (pic = it.next()) != NULL )
			queue.Push(*pic);

		queue.Close();
		stats.Wait();
	}

	if ( deinterlacedTest ) {
		cerr << "Set deinterlacing to FORCE" << endl;
		CMediaSettings::Get().GetCurrentVideoSettings().m_DeinterlaceMode = VS_DEINTERLACEMODE_FORCE;

		CDVDVideoCodecIMX codec;
		Queue queue;
		BufferIterator it(&codec, argv[1]);
		DVDVideoPicture *pic;
		Stats stats(&queue);

		queue.SetCapacity(3);
		stats.Start();

		while ( (pic = it.next()) != NULL )
			queue.Push(*pic);

		queue.Close();
		stats.Wait();
	}

	if ( progressiveRenderTest ) {
		cerr << "Set deinterlacing to OFF and render" << endl;
		CMediaSettings::Get().GetCurrentVideoSettings().m_DeinterlaceMode = VS_DEINTERLACEMODE_OFF;

		CDVDVideoCodecIMX codec;
		Queue queue;
		BufferIterator it(&codec, argv[1]);
		DVDVideoPicture *pic;
		EGL egl(&queue);

		queue.SetCapacity(3);
		egl.Start();

		while ( (pic = it.next()) != NULL )
			queue.Push(*pic);

		queue.Close();
		egl.Wait();
	}

	if ( deinterlacedRenderTest ) {
		cerr << "Set deinterlacing to FORCE and render" << endl;
		CMediaSettings::Get().GetCurrentVideoSettings().m_DeinterlaceMode = VS_DEINTERLACEMODE_FORCE;

		CDVDVideoCodecIMX codec;
		Queue queue;
		BufferIterator it(&codec, argv[1]);
		DVDVideoPicture *pic;
		EGL egl(&queue);

		queue.SetCapacity(3);
		egl.Start();

		while ( (pic = it.next()) != NULL )
			queue.Push(*pic);

		queue.Close();
		egl.Wait();
	}

	return 0;
}

