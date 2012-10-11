/*
    qarv, a Qt interface to aravis.
    Copyright (C) 2012  Jure Varlec <jure.varlec@ad-vega.si>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#ifndef ARCAM_H
#define ARCAM_H

#include <QList>
#include <QString>
#include <QRect>
#include <QPair>
#include <QMetaType>
#include <QImage>
#include <QHostAddress>
#include <QAbstractItemModel>

//! Initialize glib and aravis. Call this once in the main program.
void arcamInit();

//! \name Forward declarations to avoid exposing arv.h
/**@{*/
struct _ArvCamera;
typedef _ArvCamera ArvCamera;
struct _ArvDevice;
typedef _ArvDevice ArvDevice;
struct _ArvBuffer;
typedef _ArvBuffer ArvBuffer;
struct _ArvStream;
typedef _ArvStream ArvStream;
struct _ArvGc;
typedef _ArvGc ArvGc;
class ArFeatureTree;
/**@}*/

//! Objects of this class are used to identify cameras.
/*!
 * This class exposes three public strings containing the internal
 * Aravis id of the camera and the name of the camera vendor and model.
 * These strings are owned by the instance, not by Aravis.
 */
class ArCamId {
public:
  ArCamId();
  ArCamId(const char* id, const char* vendor, const char* model);
  ArCamId(const ArCamId& camid);
  ~ArCamId();
  const char *id, *vendor, *model;
};

Q_DECLARE_METATYPE(ArCamId)


//! ArCam provides an interface to an Aravis camera.
/*!
 * This class is mostly a thin wrapper around the arv_camera interface.
 * Only the parts that differ significantly from that interface are documented.
 * The arcamInit() function must be called once in the main program before
 * this class is used.
 */
class ArCam : public QAbstractItemModel {
  Q_OBJECT

public:
  //! A camera with the given ID is opened.
  ArCam(ArCamId id, QObject* parent = NULL);
  ~ArCam();

  static QList<ArCamId> listCameras(); //!< Returns a list of all cameras found.

  ArCamId getId(); //!< Returns the ID of the camera.

  //! \name Manipulate region of interest
  //@{
  QRect getROI();
  void setROI(QRect roi);
  QRect getROIMaxSize();
  //@}

  //! \name Manipulate pixel binning
  /**@{*/
  QSize getBinning();
  void setBinning(QSize bin);
  /**@}*/

  //! \name Choose pixel format
  /**@{*/
  /*!
   * The lists returned by ::getPixelFormat() and ::getPixelFormatNames()
   * are congruent.
   */
  QList<QString> getPixelFormats();
  QList<QString> getPixelFormatNames();
  QString getPixelFormat();
  void setPixelFormat(QString format);
  /**@}*/

  //! \name Manipulate frames-per-second.
  /**@{*/
  double getFPS();
  void setFPS(double fps);
  /**@}*/

  //! \name Manipulate exposure time (in microseconds)
  /**@{*/
  double getExposure();
  void setExposure(double exposure);
  QPair<double, double> getExposureLimits();
  bool hasAutoExposure();
  void setAutoExposure(bool enable);
  /**@}*/

  //! \name Manipulate sensor gain
  /**@{*/
  double getGain();
  void setGain(double gain);
  QPair<double, double> getGainLimits();
  bool hasAutoGain();
  void setAutoGain(bool enable);
  /**@}*/

  //! \name Start or stop acquisition
  /**@{*/
  void startAcquisition();
  void stopAcquisition();
  /**@}*/

  //! \name Get a captured frame
  /**@{*/
  QSize getFrameSize();
  QByteArray getFrame(bool dropInvalid = false);
  /**@}*/

  //! \name Manipulate network parameters of an ethernet camera
  /**@{*/
  /*!
   * MTU corresponds to "GevSCPSPacketSize", which should be set to the
   * MTU of the network interface. getHostIP() can be used to detect the
   * interface's address.
   */
  void setMTU(int mtu);
  int getMTU();
  QHostAddress getIP();
  QHostAddress getHostIP();
  int getEstimatedBW();
  /**@}*/

signals:
  //! Emitted when a new frame is ready.
  void frameReady();

private:
  void swapBuffers();
  static QList<ArCamId> cameraList;
  ArvCamera* camera;
  ArvDevice* device;
  ArvStream* stream;
  ArvBuffer* currentFrame;
  bool acquiring;

  friend void streamCallback(ArvStream* stream, ArCam* cam);

public:
  //! \name QAbstractItemModel implementation
  /**@{*/
  QModelIndex index(int row, int column,
                    const QModelIndex& parent = QModelIndex()) const;
  QModelIndex parent(const QModelIndex& index) const;
  int rowCount(const QModelIndex& parent = QModelIndex()) const;
  int columnCount(const QModelIndex& parent = QModelIndex()) const;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const;
  Qt::ItemFlags flags(const QModelIndex & index) const;
  QVariant headerData (int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const;
  /**@}*/

private:
  ArvGc* genicam;
  ArFeatureTree* featuretree;
};

//! \name Types that correspond to types of feature nodes
/**@{*/
/*!
 * These types are used by the ArCam model and delegate to edit feature
 * node values. Sometimes, a feature has several possible types (e.g. an
 * enumeration can be either an enumeration, a string or an integer; an integer
 * can be cast to a float etc.), but the delegate needs to be able to identify
 * the type exactly. Therefore, each type is given a distinct class.
 * When deciding what type to return, the model tries to match the
 * highest-level type.
 */

struct ArEnumeration {
  QList<QString> names;
  QList<QString> values;
  QList<bool> isAvailable;
  int currentValue;
  ArEnumeration(): values(), isAvailable() {}
  operator QString() { return names[currentValue]; }
};
Q_DECLARE_METATYPE(ArEnumeration)

struct ArString {
  QString value;
  qint64 maxlength;
  ArString(): value() {}
  operator QString() { return value; }
};
Q_DECLARE_METATYPE(ArString)

struct ArFloat {
  double value, min, max;
  QString unit;
  ArFloat(): unit() {}
  operator QString() { return QString::number(value) + " " + unit; }
};
Q_DECLARE_METATYPE(ArFloat)

struct ArInteger {
  qint64 value, min, max, inc;
  operator QString() { return QString::number(value); }
};
Q_DECLARE_METATYPE(ArInteger)

struct ArBoolean {
  bool value;
  operator QString() { return value ? "on/true" : "off/false"; }
};
Q_DECLARE_METATYPE(ArBoolean)

struct ArCommand {
  operator QString() { return "<command>"; }
};
Q_DECLARE_METATYPE(ArCommand)

struct ArRegister {
  QByteArray value;
  qint64 length;
  ArRegister(): value() {}
  operator QString() { return QString("0x") + value.toHex(); }
};
Q_DECLARE_METATYPE(ArRegister)

/**@}*/

#endif // ARCAM_H
