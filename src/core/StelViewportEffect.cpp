/*
 * Stellarium
 * Copyright (C) 2010 Fabien Chereau
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA  02110-1335, USA.
 */

#include "StelViewportEffect.hpp"
#include "StelApp.hpp"
#include "StelCore.hpp"
#include "StelPainter.hpp"
#include "SphericMirrorCalculator.hpp"
#include "StelFileMgr.hpp"
#include "StelMovementMgr.hpp"
#include "StelUtils.hpp"

#include <QOpenGLFramebufferObject>
#include <QSettings>
#include <QFile>
#include <QDir>

namespace
{
constexpr int SPHERIC_MIRROR_COORDS_PER_VERTEX = 2;
constexpr int SPHERIC_MIRROR_COORDS_PER_COLOR = 4;
constexpr int SPHERIC_MIRROR_COORDS_PER_TEXCOORD = 2;
enum
{
	VERTEX_ATTRIB_INDEX,
	COLOR_ATTRIB_INDEX,
	TEXCOORD_ATTRIB_INDEX,
};
}

void StelViewportEffect::paintViewportBuffer(const QOpenGLFramebufferObject* buf) const
{
	StelPainter sPainter(StelApp::getInstance().getCore()->getProjection2d());
	sPainter.setColor(1,1,1);
	sPainter.glFuncs()->glBindTexture(GL_TEXTURE_2D, buf->texture());
	sPainter.drawRect2d(0, 0, buf->size().width(), buf->size().height());
}

struct VertexPoint
{
	Vec2f ver_xy;
	Vec4f color;
	double h;
};

StelViewportDistorterFisheyeToSphericMirror::StelViewportDistorterFisheyeToSphericMirror(int screen_w,int screen_h)
	: screen_w(screen_w)
	, screen_h(screen_h)
	, originalProjectorParams(StelApp::getInstance().getCore()->getCurrentStelProjectorParams())
	, texture_point_array(Q_NULLPTR)
	, vao(new QOpenGLVertexArrayObject)
	, verticesVBO(new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer))
{
	QSettings& conf = *StelApp::getInstance().getSettings();
	StelCore* core = StelApp::getInstance().getCore();
	StelProjector::StelProjectorParams params = core->getCurrentStelProjectorParams();
	screen_h *= params.devicePixelsPerPixel;
	screen_w *= params.devicePixelsPerPixel;
	// initialize viewport parameters and texture size:

	// maximum FOV value of the not yet distorted image
	double distorter_max_fov = conf.value("spheric_mirror/distorter_max_fov",175.).toDouble();
	if (distorter_max_fov > 240.)
		distorter_max_fov = 240.;
	else if (distorter_max_fov < 120.)
		distorter_max_fov = 120.;
	if (distorter_max_fov > core->getMovementMgr()->getMaxFov())
		distorter_max_fov = core->getMovementMgr()->getMaxFov();

	StelProjectorP prj = core->getProjection(StelCore::FrameJ2000);
	core->getMovementMgr()->setMaxFov(distorter_max_fov);

	// width of the not yet distorted image
	newProjectorParams.devicePixelsPerPixel = 1;
	newProjectorParams.viewportXywh[2] = conf.value("spheric_mirror/newProjectorParams.viewportXywhWidth", originalProjectorParams.viewportXywh[2] * params.devicePixelsPerPixel).toInt();
	if (newProjectorParams.viewportXywh[2] <= 0)
	{
		newProjectorParams.viewportXywh[2] = qRound(originalProjectorParams.viewportXywh[2] * params.devicePixelsPerPixel);
	}
	else if (newProjectorParams.viewportXywh[2] > screen_w)
	{
		newProjectorParams.viewportXywh[2] = screen_w;
	}

	// height of the not yet distorted image
	newProjectorParams.viewportXywh[3] = conf.value("spheric_mirror/newProjectorParams.viewportXywhHeight", originalProjectorParams.viewportXywh[3] * params.devicePixelsPerPixel).toInt();
	if (newProjectorParams.viewportXywh[3] <= 0)
	{
		newProjectorParams.viewportXywh[3] = qRound(originalProjectorParams.viewportXywh[3] * params.devicePixelsPerPixel);
	}
	else if (newProjectorParams.viewportXywh[3] > screen_h)
	{
		newProjectorParams.viewportXywh[3] = screen_h;
	}

	// center of the FOV-disk in the not yet distorted image
	newProjectorParams.viewportCenter[0] = conf.value("spheric_mirror/viewportCenterX", 0.5*newProjectorParams.viewportXywh[2]).toDouble();
	newProjectorParams.viewportCenter[1] = conf.value("spheric_mirror/viewportCenterY", 0.5*newProjectorParams.viewportXywh[3]).toDouble();

	// diameter of the FOV-disk in pixels
	newProjectorParams.viewportFovDiameter = conf.value("spheric_mirror/viewport_fov_diameter", qMin(newProjectorParams.viewportXywh[2],newProjectorParams.viewportXywh[3])).toDouble();

	viewport_texture_offset[0] = (screen_w-newProjectorParams.viewportXywh[2])>>1;
	viewport_texture_offset[1] = (screen_h-newProjectorParams.viewportXywh[3])>>1;

	newProjectorParams.viewportXywh[0] = (screen_w-newProjectorParams.viewportXywh[2]) >> 1;
	newProjectorParams.viewportXywh[1] = (screen_h-newProjectorParams.viewportXywh[3]) >> 1;

	core->setCurrentStelProjectorParams(newProjectorParams);

	// init transformation
	VertexPoint *vertex_point_array = Q_NULLPTR;
	const QString custom_distortion_file = conf.value("spheric_mirror/custom_distortion_file","").toString();
	if (custom_distortion_file.isEmpty()) {
		float texture_triangle_base_length = conf.value("spheric_mirror/texture_triangle_base_length",16.f).toFloat();
		if (texture_triangle_base_length > 256.f) {
			texture_triangle_base_length = 256.f;
		} else if (texture_triangle_base_length < 2.f) {
			texture_triangle_base_length = 2.f;
		}
		max_x = static_cast<int>(StelUtils::trunc(0.5f + static_cast<float>(screen_w)/texture_triangle_base_length));
		step_x = screen_w / static_cast<float>(max_x-0.5);
		max_y = static_cast<int>(StelUtils::trunc(screen_h/(texture_triangle_base_length*0.5f*std::sqrt(3.0f))));
		step_y = screen_h/ static_cast<float>(max_y);

		double gamma = qMax(0.0, conf.value("spheric_mirror/projector_gamma",0.45).toDouble());

		const float view_scaling_factor = 0.5f * static_cast<float>(newProjectorParams.viewportFovDiameter) / prj->fovToViewScalingFactor(static_cast<float>(distorter_max_fov*(M_PI/360.0)));
		texture_point_array = new Vec2f[static_cast<size_t>((max_x+1)*(max_y+1))];
		vertex_point_array = new VertexPoint[static_cast<size_t>((max_x+1)*(max_y+1))];
		double max_h = 0;
		SphericMirrorCalculator calc(conf);
		for (int j=0;j<=max_y;j++) {
			for (int i=0;i<=max_x;i++) {
				VertexPoint &vertex_point(vertex_point_array[(j*(max_x+1)+i)]);
				Vec2f &texture_point(texture_point_array[(j*(max_x+1)+i)]);
				vertex_point.ver_xy[0] = ((i == 0) ? 0.f : (i == max_x) ? screen_w : (i-0.5f*(j&1))*step_x);
				vertex_point.ver_xy[1] = j*step_y;
				Vec3f v,vX,vY;
				bool rc = calc.retransform(
							  (vertex_point.ver_xy[0]-0.5f*screen_w) / screen_h,
							  (vertex_point.ver_xy[1]-0.5f*screen_h) / screen_h, v,vX,vY);
				rc &= prj->forward(v);
				const float x = static_cast<float>(newProjectorParams.viewportCenter[0]) + v[0] * view_scaling_factor;
				const float y = static_cast<float>(newProjectorParams.viewportCenter[1]) + v[1] * view_scaling_factor;
				vertex_point.h = rc ? static_cast<double>((vX^vY).norm()) : 0.0;

				// sharp image up to the border of the fisheye image, at the cost of
				// accepting clamping artefacts. You can get rid of the clamping
				// artefacts by specifying a viewport size a little less then
				// (1<<n)*(1<<n), for instance 1022*1022. With a viewport size
				// of 512*512 and viewportFovDiameter=512 you will get clamping
				// artefacts in the 3 otherwise black hills on the bottom of the image.

				//      if (x < 0.f) {x=0.f;vertex_point.h=0;}
				//      else if (x > newProjectorParams.viewportXywh[2]) {x=newProjectorParams.viewportXywh[2];vertex_point.h=0;}
				//      if (y < 0.f) {y=0.f;vertex_point.h=0;}
				//      else if (y > newProjectorParams.viewportXywh[3]) {y=newProjectorParams.viewportXywh[3];vertex_point.h=0;}

				texture_point[0] = (viewport_texture_offset[0]+x)/screen_w;
				texture_point[1] = (viewport_texture_offset[1]+y)/screen_h;

				if (vertex_point.h > max_h) max_h = vertex_point.h;
			}
		}
		for (int j=0;j<=max_y;j++) {
			for (int i=0;i<=max_x;i++) {
				VertexPoint &vertex_point(vertex_point_array[(j*(max_x+1)+i)]);
				vertex_point.color[0] = vertex_point.color[1] = vertex_point.color[2] =
					(vertex_point.h<=0.0) ? 0.0f : static_cast<float>(exp(gamma*log(vertex_point.h/max_h)));
				vertex_point.color[3] = 1.0f;
			}
		}
	} else {
		QFile file;
		QTextStream in;
		QString fName = StelFileMgr::findFile(custom_distortion_file);
		if (fName.isEmpty()) {
			qWarning() << "Could not open custom_distortion_file:" << custom_distortion_file;
		} else {
			file.setFileName(fName);
			if(file.open(QIODevice::ReadOnly))
				in.setDevice(&file);
			else
				qWarning() << "Could not open custom_distortion_file:" << custom_distortion_file;
		}
		Q_ASSERT(file.error()!=QFile::NoError);
		in >> max_x >> max_y;
		Q_ASSERT(in.status()==QTextStream::Ok && max_x>0 && max_y>0);
		step_x = screen_w / (static_cast<float>(max_x)-0.5f);
		step_y = screen_h / static_cast<float>(max_y);
		texture_point_array = new Vec2f[static_cast<size_t>((max_x+1)*(max_y+1))];
		vertex_point_array = new VertexPoint[static_cast<size_t>((max_x+1)*(max_y+1))];
		for (int j=0;j<=max_y;j++)
		{
			for (int i=0;i<=max_x;i++)
			{
				VertexPoint &vertex_point(vertex_point_array[(j*(max_x+1)+i)]);
				Vec2f &texture_point(texture_point_array[(j*(max_x+1)+i)]);
				vertex_point.ver_xy[0] = ((i == 0) ? 0.f : (i == max_x) ? screen_w : (i-0.5f*(j&1))*step_x);
				vertex_point.ver_xy[1] = static_cast<float>(j*step_y);
				float x,y;
				in >> x >> y >> vertex_point.color[0] >> vertex_point.color[1] >> vertex_point.color[2];
				vertex_point.color[3] = 1.0f;
				Q_ASSERT(in.status()!=QTextStream::Ok);
				texture_point[0] = (viewport_texture_offset[0]+x)/screen_w;
				texture_point[1] = (viewport_texture_offset[1]+y)/screen_h;
			}
		}
	}

	// initialize the display list
	displayVertexList.clear();
	for (int j=0;j<max_y;j++)
	{
		const Vec2f *t0 = texture_point_array + j*(max_x+1);
		const Vec2f *t1 = t0;
		const VertexPoint *v0 = vertex_point_array + j*(max_x+1);
		const VertexPoint *v1 = v0;
		if (j&1)
		{
			t1 += (max_x+1);
			v1 += (max_x+1);
		}
		else
		{
			t0 += (max_x+1);
			v0 += (max_x+1);
		}
		for (int i=0;i<=max_x;i++,t0++,t1++,v0++,v1++)
		{
			displayColorList << v0->color << v1->color;
			displayTexCoordList << *t0 << *t1;
			displayVertexList << v0->ver_xy << v1->ver_xy;
		}
	}
	delete[] vertex_point_array;

	StelApp::getInstance().ensureGLContextCurrent();
	setupBuffers();
	setupShaders();
}

void StelViewportDistorterFisheyeToSphericMirror::setupShaders()
{
	QOpenGLShader vsh(QOpenGLShader::Vertex);
	const auto vsrc =
		StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) +
		"ATTRIBUTE highp vec3 vertex;\n"
		"ATTRIBUTE mediump vec2 texCoord;\n"
		"ATTRIBUTE mediump vec4 color;\n"
		"uniform mediump mat4 projectionMatrix;\n"
		"VARYING mediump vec2 texc;\n"
		"VARYING mediump vec4 outColor;\n"
		"void main()\n"
		"{\n"
		"    gl_Position = projectionMatrix * vec4(vertex, 1.);\n"
		"    texc = texCoord;\n"
		"    outColor = color;\n"
		"}\n";
	vsh.compileSourceCode(vsrc);
	if (!vsh.log().isEmpty())
		qWarning().noquote() << "StelViewportDistorterFisheyeToSphericMirror: Warnings while compiling vertex shader: " << vsh.log();

	QOpenGLShader fsh(QOpenGLShader::Fragment);
	const auto fsrc =
		StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER) +
		"VARYING mediump vec2 texc;\n"
		"VARYING mediump vec4 outColor;\n"
		"uniform sampler2D tex;\n"
		"void main()\n"
		"{\n"
		"    FRAG_COLOR = texture2D(tex, texc)*outColor;\n"
		"}\n";
	fsh.compileSourceCode(fsrc);
	if (!fsh.log().isEmpty())
		qWarning().noquote() << "StelViewportDistorterFisheyeToSphericMirror: Warnings while compiling fragment shader: " << fsh.log();

	shaderProgram.reset(new QOpenGLShaderProgram(QOpenGLContext::currentContext()));
	shaderProgram->addShader(&vsh);
	shaderProgram->addShader(&fsh);
	shaderProgram->bindAttributeLocation("vertex", VERTEX_ATTRIB_INDEX);
	shaderProgram->bindAttributeLocation("color", COLOR_ATTRIB_INDEX);
	shaderProgram->bindAttributeLocation("texCoord", TEXCOORD_ATTRIB_INDEX);
	StelPainter::linkProg(shaderProgram.get(), "spheric mirror distortion shader program");
	shaderVars.projectionMatrix = shaderProgram->uniformLocation("projectionMatrix");
	shaderVars.texture = shaderProgram->uniformLocation("tex");
}

void StelViewportDistorterFisheyeToSphericMirror::setupBuffers()
{
	verticesVBO->create();
	verticesVBO->setUsagePattern(QOpenGLBuffer::StaticDraw);
	verticesVBO->bind();
	const int colorElemSize = SPHERIC_MIRROR_COORDS_PER_COLOR * sizeof(GLfloat);
	const int vertElemSize = SPHERIC_MIRROR_COORDS_PER_VERTEX * sizeof(GLfloat);
	const int texCoordElemSize = SPHERIC_MIRROR_COORDS_PER_TEXCOORD * sizeof(GLfloat);
	const int numberOfVerticesToCopy = displayVertexList.size();
	const auto bufferSize = vertElemSize*numberOfVerticesToCopy +
	                        texCoordElemSize*numberOfVerticesToCopy +
	                        colorElemSize*numberOfVerticesToCopy;
	verticesVBO->allocate(bufferSize);
	vboVertexDataOffset = 0;
	const auto vertexDataSize = vertElemSize*numberOfVerticesToCopy;
	verticesVBO->write(vboVertexDataOffset, displayVertexList.constData(), vertexDataSize);
	vboTexCoordDataOffset = vertexDataSize;
	const auto texCoordDataSize = texCoordElemSize*numberOfVerticesToCopy;
	verticesVBO->write(vboTexCoordDataOffset, displayTexCoordList.constData(), texCoordDataSize);
	vboColorDataOffset = vertexDataSize + texCoordDataSize;
	const auto colorDataSize = colorElemSize*numberOfVerticesToCopy;
	verticesVBO->write(vboColorDataOffset, displayColorList.constData(), colorDataSize);
	verticesVBO->release();

	vao->create();
	bindVAO();
	setupCurrentVAO();
	releaseVAO();
}

void StelViewportDistorterFisheyeToSphericMirror::setupCurrentVAO() const
{
	auto& gl = *QOpenGLContext::currentContext()->functions();
	verticesVBO->bind();
	gl.glVertexAttribPointer(VERTEX_ATTRIB_INDEX, SPHERIC_MIRROR_COORDS_PER_VERTEX, GL_FLOAT,
	                         false, 0, reinterpret_cast<const void*>(uintptr_t(vboVertexDataOffset)));
	gl.glVertexAttribPointer(COLOR_ATTRIB_INDEX, SPHERIC_MIRROR_COORDS_PER_COLOR, GL_FLOAT,
	                         false, 0, reinterpret_cast<const void*>(uintptr_t(vboColorDataOffset)));
	gl.glVertexAttribPointer(TEXCOORD_ATTRIB_INDEX, SPHERIC_MIRROR_COORDS_PER_TEXCOORD, GL_FLOAT,
	                         false, 0, reinterpret_cast<const void*>(uintptr_t(vboTexCoordDataOffset)));
	verticesVBO->release();
	gl.glEnableVertexAttribArray(VERTEX_ATTRIB_INDEX);
	gl.glEnableVertexAttribArray(COLOR_ATTRIB_INDEX);
	gl.glEnableVertexAttribArray(TEXCOORD_ATTRIB_INDEX);
}

void StelViewportDistorterFisheyeToSphericMirror::bindVAO() const
{
	if(vao->isCreated())
		vao->bind();
	else
		setupCurrentVAO();
}

void StelViewportDistorterFisheyeToSphericMirror::releaseVAO() const
{
	if(vao->isCreated())
	{
		vao->release();
	}
	else
	{
		auto& gl = *QOpenGLContext::currentContext()->functions();
		gl.glDisableVertexAttribArray(VERTEX_ATTRIB_INDEX);
		gl.glDisableVertexAttribArray(COLOR_ATTRIB_INDEX);
		gl.glDisableVertexAttribArray(TEXCOORD_ATTRIB_INDEX);
	}
}


StelViewportDistorterFisheyeToSphericMirror::~StelViewportDistorterFisheyeToSphericMirror(void)
{
	StelApp::getInstance().ensureGLContextCurrent(); // Make sure that VBO & VAO will be properly deleted

	if (texture_point_array)
		delete[] texture_point_array;
	StelApp::getInstance().getCore()->setCurrentStelProjectorParams(originalProjectorParams);
}


void StelViewportDistorterFisheyeToSphericMirror::distortXY(qreal &x, qreal &y) const
{
	float texture_x,texture_y;

	// Input positions are not scaled, so we do it here
	// Since the effect fbo uses pixel coordinates.
	x *= originalProjectorParams.devicePixelsPerPixel;
	y *= originalProjectorParams.devicePixelsPerPixel;

	// find the triangle and interpolate accordingly:
	float dy = static_cast<float>(y) / step_y;
	const int j = static_cast<int>(floorf(dy));
	dy -= j;
	if (j&1)
	{
		float dx = static_cast<float>(x) / step_x + 0.5f*(1.f-dy);
		const int i = static_cast<int>(floorf(dx));
		dx -= i;
		const Vec2f *const t = texture_point_array + (j*(max_x+1)+i);
		if (dx + dy <= 1.f)
		{
			if (i == 0)
			{
				dx -= 0.5f*(1.f-dy);
				dx *= 2.f;
			}
			texture_x = t[0][0] + dx * (t[1][0]-t[0][0]) + dy * (t[max_x+1][0]-t[0][0]);
			texture_y = t[0][1] + dx * (t[1][1]-t[0][1]) + dy * (t[max_x+1][1]-t[0][1]);
		}
		else
		{
			if (i == max_x-1)
			{
				dx -= 0.5f*(1.f-dy);
				dx *= 2.f;
			}
			texture_x = t[max_x+2][0] + (1.f-dy) * (t[1][0]-t[max_x+2][0]) + (1.f-dx) * (t[max_x+1][0]-t[max_x+2][0]);
			texture_y = t[max_x+2][1] + (1.f-dy) * (t[1][1]-t[max_x+2][1]) + (1.f-dx) * (t[max_x+1][1]-t[max_x+2][1]);
		}
	}
	else
	{
		float dx = static_cast<float>(x) / step_x + 0.5f*dy;
		const int i = static_cast<int>(floorf(dx));
		dx -= i;
		const Vec2f *const t = texture_point_array + (j*(max_x+1)+i);
		if (dx >= dy)
		{
			if (i == max_x-1)
			{
				dx -= 0.5f*dy;
				dx *= 2.f;
			}
			texture_x = t[1][0] + (1.f-dx) * (t[0][0]-t[1][0]) + dy * (t[max_x+2][0]-t[1][0]);
			texture_y = t[1][1] + (1.f-dx) * (t[0][1]-t[1][1]) + dy * (t[max_x+2][1]-t[1][1]);
		}
		else
		{
			if (i == 0)
			{
				dx -= 0.5f*dy;
				dx *= 2.f;
			}
			texture_x = t[max_x+1][0] + (1.f-dy) * (t[0][0]-t[max_x+1][0]) + dx * (t[max_x+2][0]-t[max_x+1][0]);
			texture_y = t[max_x+1][1] + (1.f-dy) * (t[0][1]-t[max_x+1][1]) + dx * (t[max_x+2][1]-t[max_x+1][1]);
		}
	}

	x = static_cast<double>(screen_w*texture_x) - viewport_texture_offset[0] + newProjectorParams.viewportXywh[0];
	y = static_cast<double>(screen_h*texture_y) - viewport_texture_offset[1] + newProjectorParams.viewportXywh[1];
}


void StelViewportDistorterFisheyeToSphericMirror::paintViewportBuffer(const QOpenGLFramebufferObject* buf) const
{
	const auto prj = StelApp::getInstance().getCore()->getProjection2d();
	auto& gl = *QOpenGLContext::currentContext()->functions();
	const int texUnit = 0;
	GL(gl.glActiveTexture(GL_TEXTURE0 + texUnit));
	GL(gl.glBindTexture(GL_TEXTURE_2D, buf->texture()));
	GL(gl.glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
	GL(gl.glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));

	shaderProgram->bind();
	const Mat4f& m = prj->getProjectionMatrix();
	const QMatrix4x4 qMat(m[0], m[4], m[8], m[12], m[1], m[5], m[9], m[13], m[2], m[6], m[10], m[14], m[3], m[7], m[11], m[15]);
	shaderProgram->setUniformValue(shaderVars.projectionMatrix, qMat);
	shaderProgram->setUniformValue(shaderVars.texture, texUnit);

	bindVAO();
	for (int j=0;j<max_y;j++)
	{
		GL(gl.glDrawArrays(GL_TRIANGLE_STRIP, j*(max_x+1)*2, (max_x+1)*2));
	}
	releaseVAO();

	shaderProgram->release();

	GL(gl.glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
	GL(gl.glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
}

