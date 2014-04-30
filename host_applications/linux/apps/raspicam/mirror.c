/*
Copyright (c) 2013, Broadcom Europe Ltd
Copyright (c) 2013, Tim Gover
All rights reserved.


Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "RaspiTex.h"
#include "RaspiTexUtil.h"
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#define SHADER_MAX_ATTRIBUTES 16
#define SHADER_MAX_UNIFORMS   16

/**
 * Container for a GL texture.
 */
struct TEXTURE_T {
    GLuint name;
    GLuint width;
    GLuint height;
};

/**
 * Container for a simple vertex, fragment share with the names and locations.
 */
struct SHADER_PROGRAM_T {
    const char *vertex_source;
    char *fragment_source;
    const char *uniform_names[SHADER_MAX_UNIFORMS];
    const char *attribute_names[SHADER_MAX_ATTRIBUTES];
    GLint vs;
    GLint fs;
    GLint program;

    /// The locations for uniforms defined in uniform_names
    GLint uniform_locations[SHADER_MAX_UNIFORMS];

    /// The locations for attributes defined in attribute_names
    GLint attribute_locations[SHADER_MAX_ATTRIBUTES];

    /// Optional texture information
    struct TEXTURE_T tex;
};

/**
 * Draws an external EGL image and applies a sine wave distortion to create
 * a hall of mirrors effect.
 */
struct SHADER_PROGRAM_T picture_shader = {
    .vertex_source =
    "attribute vec2 vertex;\n"
    "varying vec2 texcoord;"
    "void main(void) {\n"
    "   texcoord = 0.5 * (vertex + 1.0);\n"
    "   gl_Position = vec4(vertex, 0.0, 1.0);\n"
    "}\n",

    .fragment_source = NULL,
    .uniform_names = {"tex", "segment_u", "segment_v"},
    .attribute_names = {"vertex"},

};

/**
 * Utility for building shaders and configuring the attribute and locations.
 * @return Zero if successful.
 */
static int buildShaderProgram(struct SHADER_PROGRAM_T *p)
{
    GLint status;
    int i = 0;
    char log[1024];
    int logLen = 0;
    assert(p);
    assert(p->vertex_source);
    assert(p->fragment_source);

    if (! (p && p->vertex_source && p->fragment_source))
        goto fail;

    p->vs = p->fs = 0;

    GLCHK(p->vs = glCreateShader(GL_VERTEX_SHADER));
    GLCHK(glShaderSource(p->vs, 1, &p->vertex_source, NULL));
    GLCHK(glCompileShader(p->vs));
    GLCHK(glGetShaderiv(p->vs, GL_COMPILE_STATUS, &status));
    if (! status) {
        glGetShaderInfoLog(p->vs, sizeof(log), &logLen, log);
        vcos_log_trace("Program info log %s", log);
        goto fail; // lock computer
    }

    GLCHK(p->fs = glCreateShader(GL_FRAGMENT_SHADER));
    GLCHK(glShaderSource(p->fs, 1, (const char**)&p->fragment_source, NULL));
    GLCHK(glCompileShader(p->fs));
    GLCHK(glGetShaderiv(p->fs, GL_COMPILE_STATUS, &status));
    if (! status) {
        glGetShaderInfoLog(p->fs, sizeof(log), &logLen, log);
        vcos_log_trace("Program info log %s", log);
        goto fail;
    }

    GLCHK(p->program = glCreateProgram());
    GLCHK(glAttachShader(p->program, p->vs));
    GLCHK(glAttachShader(p->program, p->fs));
    GLCHK(glLinkProgram(p->program));

    GLCHK(glGetProgramiv(p->program, GL_LINK_STATUS, &status));
    if (! status)
    {
        vcos_log_trace("Failed to link shader program");
        glGetProgramInfoLog(p->program, sizeof(log), &logLen, log);
        vcos_log_trace("Program info log %s", log);
        goto fail;
    }

    for (i = 0; i < SHADER_MAX_ATTRIBUTES; ++i)
    {
        if (! p->attribute_names[i])
            break;
        GLCHK(p->attribute_locations[i] = glGetAttribLocation(p->program, p->attribute_names[i]));
        if (p->attribute_locations[i] == -1)
        {
            vcos_log_trace("Failed to get location for attribute %s", p->attribute_names[i]);
            goto fail;
        }
        else {
            vcos_log_trace("Attribute for %s is %d", p->attribute_names[i], p->attribute_locations[i]);
        }
    }

    for (i = 0; i < SHADER_MAX_UNIFORMS; ++i)
    {
        if (! p->uniform_names[i])
            break;
        GLCHK(p->uniform_locations[i] = glGetUniformLocation(p->program, p->uniform_names[i]));
        if (p->uniform_locations[i] == -1)
        {
            vcos_log_trace("Failed to get location for uniform %s", p->uniform_names[i]);
            goto fail;
        }
        else {
            vcos_log_trace("Uniform for %s is %d", p->uniform_names[i], p->uniform_locations[i]);
        }
    }

    return 0;
fail:
    vcos_log_trace("%s: Failed to build shader program", __func__);
    if (p)
    {
        glDeleteProgram(p->program);
        glDeleteShader(p->fs);
        glDeleteShader(p->vs);
    }
    return -1;
}

/**
 * Creates the OpenGL ES 2.X context and builds the shaders.
 * @param raspitex_state A pointer to the GL preview state.
 * @return Zero if successful.
 */
static int mirror_init(RASPITEX_STATE *state)
{
    int rc = raspitexutil_gl_init_2_0(state);
    if (rc != 0)
       goto end;

    FILE* fragfile = fopen("/home/pi/userland/host_applications/linux/apps/raspicam/shader_segment.txt", "r");
    fseek(fragfile, 0L, SEEK_END);
    int size = ftell(fragfile);		
    fseek(fragfile, 0L, SEEK_SET);	   
    picture_shader.fragment_source = (char*)malloc(size+1);
    fread(picture_shader.fragment_source, 1, size, fragfile);
    picture_shader.fragment_source[size] = '\0';
    rc = buildShaderProgram(&picture_shader);
end:
    return rc;
}

struct FBOInfo {
  GLuint id;
  GLuint color;
  GLuint depth;
};

#define N_THETA 360
#define MAX(a,b) ((a) > (b) ? (a) : (b))

typedef struct LocMax {
  int count;
  int t;
  int r;
  int search_n;
  int search_x;
  int search_y;
  int played_at;
  int reset_at;
  int instrument;
  int play;
  int noisy;
} LocMax;

#define N_MAXES 50
LocMax maxes[N_MAXES]= {0,};
int locmax_comp(const void* c1, const void* c2) { return ((LocMax*)c1)->count-((LocMax*)c2)->count; }

static float seg_u, seg_v;

static void remove_dups() {
  int i, j;
  for(i=0 ; i<N_MAXES ; i++) {
    for(j=i+1 ; j<N_MAXES ; j++) {
      float diff=0;
      diff += abs(maxes[i].r - maxes[j].r);
      diff += abs(maxes[i].t - maxes[j].t);
      if(diff < 10) {
	maxes[i].search_n = 0;
	printf("#");
	break;
      }
    }
  }
}

static void gauss(int* data, int w, int h) {
  int x,y,kx,ky;
  float sum;
  float kernel[9] = {1.0/16,2.0/16,1.0/16,2.0/16,4.0/16,2.0/16,1.0/16,2.0/16,1.0/16};
  float* kptr;
  int* copy=(int*)malloc(w*h*sizeof(int));
  for(y=1 ; y<h-1 ; y++) {
    for(x=1 ; x<w-1 ; x++) {
      kptr=kernel;
      sum=0;
      for(ky=-1 ; ky<=1 ; ky++) {
	for(kx=-1 ; kx<=1 ;kx++) {
	  sum+=(*kptr)*data[(y-ky)*w+x-kx];
	}
      }
      copy[(y)*w+x] = (int)sum;
    }
  }
  memcpy(data,copy,w*h*sizeof(int));
  free(copy);
}
static int mirror_redraw(RASPITEX_STATE *raspitex_state) {
    static float offset = 0.0;

    //struct FBOInfo fboInfo;
    //apa    glGenFramebuffers(1, &fboInfo.id);

    // Start with a clear screen
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Bind the OES texture which is used to render the camera preview
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, raspitex_state->texture);

    offset += 0.05;
    GLCHK(glUseProgram(picture_shader.program));
    GLCHK(glEnableVertexAttribArray(picture_shader.attribute_locations[0]));
    GLfloat varray[] = {
        -1.0f, -1.0f,
        1.0f,  1.0f,
        1.0f, -1.0f,

        -1.0f,  1.0f,
        1.0f,  1.0f,
        -1.0f, -1.0f,
    };
    GLCHK(glVertexAttribPointer(picture_shader.attribute_locations[0], 2, GL_FLOAT, GL_FALSE, 0, varray));
    GLCHK(glUniform1f(picture_shader.uniform_locations[1], seg_u));
    GLCHK(glUniform1f(picture_shader.uniform_locations[2], seg_v));
    GLCHK(glDrawArrays(GL_TRIANGLES, 0, 6));

    
    int plot_i;
    for(plot_i=0 ; plot_i<N_MAXES ; plot_i++) {
      if(maxes[plot_i].search_n > 0) {
	float px = 2.0*maxes[plot_i].search_x / raspitex_state->width - 1.0;
	float py = 2.0*maxes[plot_i].search_y / raspitex_state->height - 1.0;
	float pw = 15.0/raspitex_state->width;
	float ph = 15.0/raspitex_state->height;
	/*      	GLfloat varray2[12];
	varray2[0] = px-pw; varray2[1] = py-ph;
	varray2[2] = px+pw; varray2[3] = py+ph;
	varray2[4] = px+pw; varray2[5] = py-ph;
	varray2[6] = px-pw; varray2[7] = py+ph;
	varray2[8] = px+pw; varray2[9] = py+ph;
	varray2[10] = px-pw; varray2[11] = py-ph;
	*/
	float tuta = -M_PI/2 + maxes[plot_i].t * 2.0 * M_PI / N_THETA;
	GLfloat varray2[12];
	varray2[0] = px-pw; varray2[1] = py-ph;
	varray2[2] = px+pw; varray2[3] = py-ph;
	varray2[4] = px+pw; varray2[5] = py+ph;
	varray2[6] = px-pw; varray2[7] = py+ph;
	varray2[8] = px; varray2[9] = py;
	varray2[10] = px+cos(tuta)*pw*5; varray2[11] = py+sin(tuta)*pw*5;
	
	GLCHK(glVertexAttribPointer(picture_shader.attribute_locations[0], 2, GL_FLOAT, GL_FALSE, 0, varray2));
	GLCHK(glUniform1f(picture_shader.uniform_locations[1], -1.0));
	GLCHK(glUniform1f(picture_shader.uniform_locations[2], seg_v));
	GLCHK(glDrawArrays(GL_LINES, 0, 4));
	//    GLCHK(glDrawArrays(GL_TRIANGLES, 0, 6));
      }
    }
    
    GLCHK(glDisableVertexAttribArray(picture_shader.attribute_locations[0]));
    GLCHK(glUseProgram(0));

    static int n=0;

    static int frame=0;
    static int initialized=0;

    frame++;
    if(frame % 30 == 0) {
      FILE* f = fopen("/home/pi/uv.txt", "r");
      fscanf(f, "%f %f", &seg_u, &seg_v);
      fclose(f);
    }

    if(frame < 150) {
      printf("\rStarting up: %3.0f%%", frame/150.0*100);
      fflush(stdout);
    }
    else {
      if(!initialized) {
	int w=raspitex_state->width;
	int h=raspitex_state->height;
	char* data = (char*)malloc(4*w*h);
	char* ptr = data;
	glReadPixels (0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, data);
	int x, y, t, r;

	int N_R = MAX(w,h);
	int* hough = (int*)calloc(N_THETA*N_R,sizeof(int));
	
	for(y=0 ; y<h ; y++) {
	  int npix=0;
	  for(x=0 ; x<w ; x++) {
	    char a = ptr[3];
	    if (a>0) {
	      npix++;
	      for(t=0 ; t<N_THETA ; t++) {
		float theta = 2.0*M_PI*t/N_THETA;
		int r = (int)(x*cos(theta)+y*sin(theta));
		if(r>0) {
		  hough[t*N_R+r]++;
		}
	      }
	    }
	    ptr += 4;
	  }
	  if(y%10 == 0) {
	    printf("\rHough lines: %3d%%", 100*y/h); 
	    fflush(stdout);
	  }
	}
	printf("\n");
	//	gauss(hough,w,h);

	int min_max=0;
	int min_max_i=0;

	for(t=1 ; t<N_THETA ; t++) {
	  for(r=1 ; r<N_R ; r++) {	   
	    int n = hough[t*N_R+r];
	    if(n > min_max) {

	      int n1=hough[(t-1)*N_R+r];
	      int n2=hough[(t+1)*N_R+r];
	      int n3=hough[t*N_R+(r-1)];
	      int n4=hough[t*N_R+(r+1)];
	      int n5=hough[(t-1)*N_R+(r-1)];
	      int n6=hough[(t+1)*N_R+(r-1)];
	      int n7=hough[(t-1)*N_R+(r+1)];
	      int n8=hough[(t+1)*N_R+(r+1)];
	      if(n>=n1 && n>=n2 && n>=n3 && n>=n4 && n>=n5 && n>=n6 && n>=n7 && n>=n8) {

		min_max = maxes[min_max_i].count = hough[t*N_R+r];
		maxes[min_max_i].r=r;
		maxes[min_max_i].t=t;
	      
		int i;
		for(i=0;i<N_MAXES;i++) {
		  if(maxes[i].count < min_max) {
		    min_max = maxes[min_max_i].count;
		    min_max_i = i;
		  }
		}
	      }
	    }	       
	  }
	}
	qsort(maxes, N_MAXES, sizeof(LocMax), &locmax_comp);	
	for(min_max_i=0 ; min_max_i<N_MAXES ; min_max_i++) {
	  maxes[min_max_i].instrument = -1;
	  maxes[min_max_i].noisy = 0;
	  maxes[min_max_i].played_at = 0;
	  maxes[min_max_i].reset_at = 0;
	  maxes[min_max_i].search_n = 0;
	  maxes[min_max_i].search_x = -1;
	  maxes[min_max_i].search_y = -1;
	  int sx;
	  int found=0;
	  for(sx=2 ; sx < w-2 ; sx+=10) {
	    float th = maxes[min_max_i].t*2*M_PI/(float)N_THETA;
	    int sy = -cos(th)*sin(th)*sx+maxes[min_max_i].r/sin(th);
	    if(sy > 2 && sy < h-2) {
	      int npix=0;
	      for(y=sy-2 ; y<=sy+2 ; y++) {
		for(x=sx-2 ; x<=sx+2 ; x++) {
		  char a = data[4*(w*y+x)+3];
		  if(a>0) {
		    npix++;
		  }
		}
	      }
	      if(npix>1) {
		found++;
	      }
	      if(npix > maxes[min_max_i].search_n) {
		maxes[min_max_i].search_n = npix;
		maxes[min_max_i].search_x = sx;
		maxes[min_max_i].search_y = sy;
	      }
	      //	      printf("%d ", npix);
	    }
	  }
	  if(found < 2 || maxes[min_max_i].search_n < 4) {
	    maxes[min_max_i].search_n = 0;
	    printf("@");
	  }

	  //adjust to center of mass
	  if(maxes[min_max_i].search_n) {
	    int xs = 0;
	    int ys = 0;
	    for(y=maxes[min_max_i].search_y-2 ; y<=maxes[min_max_i].search_y+2 ; y++) {
	      for(x=maxes[min_max_i].search_x-2 ; x<=maxes[min_max_i].search_x+2 ; x++) {
		char a = data[4*(w*y+x)+3];
		if(a>0) {
		  xs+=x;
		  ys+=y;
		}
	      }
	    }
	    maxes[min_max_i].search_x = xs/maxes[min_max_i].search_n;
	    maxes[min_max_i].search_y = ys/maxes[min_max_i].search_n;
	  }

	  printf("n=%d r=%d t=%d sx=%d sy=%d\n", maxes[min_max_i].count, maxes[min_max_i].r, maxes[min_max_i].t*360/N_THETA, maxes[min_max_i].search_x, maxes[min_max_i].search_y);
	}

	remove_dups();
	free(hough);
	free(data);
	initialized=1;
      }
     
      
      int line;
      //look for lines not present at search_x/y
      for(line=0 ; line<N_MAXES ; line++) {
	if(maxes[line].search_n == 0) {
	  continue;
	}
	int i;
	int r=0,g=0,b=0;
	int w=9;
	int h=9;
	char readdata[4*w*h];
	glReadPixels (maxes[line].search_x-4, maxes[line].search_y-4, w, h, GL_RGBA, GL_UNSIGNED_BYTE, readdata);
	int npixels = 0;
	for(i=0 ; i<w*h ;i++) {
	  if(readdata[i*4+3] > 0) {
	    npixels++;
	    r+=readdata[i*4];
	    g+=readdata[i*4+1];
	    b+=readdata[i*4+2];
	  }
	}
	/*	if(npixels > 4 && frame%30 == 0) {
	  float k=1.0/(256*npixels);
	  float v = ((0.439 * r) - (0.368 * g) - (0.071 * b))*k + 0.5;
	  float u = (-(0.148 * r) - (0.291 * g) + (0.439 * b))*k + 0.5;	  
	  printf("%3d %3d %3d   %1.2f %1.2f\n", r/npixels, g/npixels, b/npixels, u, v);
	  }*/

	if(npixels < 2) {
	  if (frame - maxes[line].played_at > 5 &&
	      frame - maxes[line].reset_at  < 2) {
	    printf("%d\n",n++);
	    if(maxes[line].instrument < 0) {
	      maxes[line].instrument = n;
	    }
	    maxes[line].played_at = frame;

	    if(maxes[line].noisy == 0) {
	      maxes[line].play = 1;
	    }
	    maxes[line].noisy += 10;
	  }
	}
	else {
	  maxes[line].noisy = maxes[line].noisy == 0 ? 0 : maxes[line].noisy-1; 
	  maxes[line].reset_at = frame;
	}
      }

      //play
      FILE* f = NULL;
      for(line=0 ; line<N_MAXES ; line++) {
	if(maxes[line].play) {
	  maxes[line].play = 0;
	  if(!f) {
	    f = fopen("/home/pi/plingdir/pling", "w");
	  }
	  fprintf(f, "%d",maxes[line].instrument);
	  break;
	}
      }
      if(f) {
	fclose(f);
      }

    }

    return 0;
}

int mirror_open(RASPITEX_STATE *state)
{
   state->ops.gl_init = mirror_init;
   state->ops.redraw = mirror_redraw;
   return 0;
}
