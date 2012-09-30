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

#include "mainwindow.h"

#include <QDebug>
#include <QNetworkInterface>
#include <QFileInfo>
#include <QFileDialog>
#include <QSettings>
#include <qdatetime.h>

#include "getmtu_linux.h"

const int slidersteps = 1000;
const int sliderUpdateMsec = 300;

MainWindow::MainWindow():
  QMainWindow(), camera(NULL), playing(false), recording(false),
  started(false), recordingfile(NULL), decoder(NULL),
  imageTransform(), framecounter(0), currentFrame() {

  qDebug() << "Please ignore \"Could not resolve property\" warnings "
           "unless icons look bad.";
  setupUi(this);

  // Setup theme icons if available.
  QMap<QAbstractButton*, QString> icons;
  icons[showVideoButton] = "video-display";
  icons[unzoomButton] = "zoom-original";
  icons[recordButton] = "media-record";
  icons[playButton] = "media-playback-start";
  icons[refreshCamerasButton] = "view-refresh";
  icons[chooseFilenameButton] = "document-open";
  icons[snapButton] = "document-save";
  icons[chooseSnappathButton] = "document-open";
  icons[editGainButton] = "edit-clear";
  icons[editExposureButton] = "edit-clear";
  for (auto i = icons.begin(); i != icons.end(); i++)
    i.key()->setIcon(QIcon::fromTheme(*i, QIcon(QString(":/icons/") + *i + ".svgz")));

  QSettings settings;
  restoreGeometry(settings.value("mainwindow/geometry").toByteArray());
  restoreState(settings.value("mainwindow/state").toByteArray());

  autoreadexposure = new QTimer(this);
  autoreadexposure->setInterval(sliderUpdateMsec);
  this->connect(autoreadexposure, SIGNAL(timeout()), SLOT(readExposure()));
  this->connect(autoreadexposure, SIGNAL(timeout()), SLOT(readGain()));
  this->connect(autoreadexposure, SIGNAL(timeout()), SLOT(updateBandwidthEstimation()));

  video->connect(pickROIButton, SIGNAL(toggled(bool)), SLOT(enableSelection(bool)));
  this->connect(video, SIGNAL(selectionComplete(QRect)), SLOT(pickedROI(QRect)));

  rotationSelector->addItem("No rotation", 0);
  rotationSelector->addItem("90 degrees", 90);
  rotationSelector->addItem("180 degrees", 180);
  rotationSelector->addItem("270 degrees", 270);
  this->connect(rotationSelector,
                SIGNAL(currentIndexChanged(int)),
                SLOT(updateImageTransform()));
  this->connect(invertColors, SIGNAL(stateChanged(int)), SLOT(updateImageTransform()));
  this->connect(flipHorizontal, SIGNAL(stateChanged(int)), SLOT(updateImageTransform()));
  this->connect(flipVertical, SIGNAL(stateChanged(int)), SLOT(updateImageTransform()));

  auto timer = new QTimer(this);
  timer->setInterval(1000);
  this->connect(timer, SIGNAL(timeout()), SLOT(showFPS()));
  timer->start();

  QTimer::singleShot(300, this, SLOT(on_refreshCamerasButton_clicked()));
}

void MainWindow::on_refreshCamerasButton_clicked(bool clicked) {
  cameraSelector->blockSignals(true);
  cameraSelector->clear();
  cameraSelector->setEnabled(false);
  cameraSelector->addItem("Looking for cameras...");
  QApplication::processEvents();
  QApplication::flush();
  cameraSelector->clear();
  auto cameras = ArCam::listCameras();
  foreach (auto cam, cameras) {
    QString display;
    display = display + cam.vendor + " (" + cam.model + ")";
    cameraSelector->addItem(display, QVariant::fromValue<ArCamId>(cam));
  }
  cameraSelector->setEnabled(true);
  cameraSelector->blockSignals(false);
  cameraSelector->setCurrentIndex(0);
  on_cameraSelector_currentIndexChanged(0);
}

void MainWindow::on_unzoomButton_toggled(bool checked) {
  if (checked) {
    QSize newsize = video->getImage().size();
    video->setFixedSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    video->resize(newsize);
    video->setFixedSize(newsize);
  } else {
    video->setFixedSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    video->setMinimumSize(QSize(64, 64));
  }
}

static inline double slider2value_log(int slidervalue,
                                      QPair<double, double>& range) {
  double value = log2(range.second) - log2(range.first);
  return exp2(value * slidervalue / slidersteps + log2(range.first));
}

static inline int value2slider_log(double value,
                                   QPair<double, double>& range) {
  return slidersteps *
         (log2(value) - log2(range.first)) /
         (log2(range.second) - log2(range.first));
}

static inline double slider2value(int slidervalue,
                                      QPair<double, double>& range) {
  return range.first + (range.second - range.first) *
         slidervalue / slidersteps;
}

static inline int value2slider(double value,
                                   QPair<double, double>& range) {
  return (value - range.first) / (range.second - range.first) * slidersteps;
}

void MainWindow::readROILimits() {
  auto roisize = camera->getROIMaxSize();
  roirange = QRect(QPoint(0,0), roisize.size());
  xSpinbox->setRange(0, roisize.width());
  ySpinbox->setRange(0, roisize.height());
  wSpinbox->setRange(roisize.x(), roisize.width());
  hSpinbox->setRange(roisize.y(), roisize.height());
}

void MainWindow::on_cameraSelector_currentIndexChanged(int index) {
  autoreadexposure->stop();

  auto camid = cameraSelector->itemData(index).value<ArCamId>();
  if (camera != NULL) {
    startVideo(false);
    delete camera;
  }
  camera = new ArCam(camid);
  this->connect(camera, SIGNAL(frameReady()), SLOT(takeNextFrame()));

  fpsSpinbox->setValue(camera->getFPS());

  auto ifaceIP = camera->getHostIP();
  QNetworkInterface cameraIface;
  if (!ifaceIP.isNull()) {
    auto ifaces = QNetworkInterface::allInterfaces();
    bool process_loop = true;
    foreach (QNetworkInterface iface, ifaces) {
      if (!process_loop) break;
      auto addresses = iface.addressEntries();
      foreach (QNetworkAddressEntry addr, addresses) {
        if (addr.ip() == ifaceIP) {
          cameraIface = iface;
          process_loop = false;
          break;
        }
      }
    }

    if(cameraIface.isValid()) {
      int mtu = getMTU(cameraIface.name());
      camera->setMTU(mtu);
    }
  } else {
    // No ip found; try best effort MTU in case it's still an eth cam
    camera->setMTU(1500);
  }

  if (camera->getMTU() == 0)
    cameraMTUDescription->setText("Not an ethernet camera.");
  else {
    int mtu = camera->getMTU();
    QString ifname = cameraIface.name();
    QString description = "Camera on interface " + ifname +
                          ",\nMTU set to " + QString::number(mtu) + ".";
    if (mtu < 3000)
      description += "\nConsider increasing the MTU!";
    cameraMTUDescription->setText(description);
  }

  auto formats = camera->getPixelFormats();
  auto formatnames = camera->getPixelFormatNames();
  int noofframes = formats.length();
  pixelFormatSelector->blockSignals(true);
  pixelFormatSelector->clear();
  for (int i=0; i<noofframes; i++)
    pixelFormatSelector->addItem(formatnames.at(i), formats.at(i));
  auto format = camera->getPixelFormat();
  pixelFormatSelector->setCurrentIndex(pixelFormatSelector->findData(format));
  pixelFormatSelector->setEnabled(noofframes > 1);
  pixelFormatSelector->blockSignals(false);

  on_resetROIButton_clicked(true);
  QSize binsize = camera->getBinning();
  binSpinBox->setValue(binsize.width());

  gainrange = camera->getGainLimits();
  exposurerange = camera->getExposureLimits();
  gainSlider->setRange(0, slidersteps);
  exposureSlider->setRange(0, slidersteps);
  gainSpinbox->setRange(gainrange.first, gainrange.second);
  exposureSpinbox->setRange(exposurerange.first, exposurerange.second);
  readGain();
  readExposure();

  gainAutoButton->setEnabled(camera->hasAutoGain());
  exposureAutoButton->setEnabled(camera->hasAutoExposure());
  camera->setAutoGain(false);
  camera->setAutoExposure(false);

  autoreadexposure->start();
}

void MainWindow::readExposure() {
  bool blocked = exposureSlider->blockSignals(true);
  exposureSlider->setValue(value2slider_log(camera->getExposure(),
                                            exposurerange));
  exposureSlider->blockSignals(blocked);
  exposureSpinbox->setValue(camera->getExposure());
}

void MainWindow::readGain() {
  bool blocked = gainSlider->blockSignals(true);
  gainSlider->setValue(value2slider(camera->getGain(), gainrange));
  gainSlider->blockSignals(blocked);
  gainSpinbox->setValue(camera->getGain());
}

void MainWindow::on_exposureSlider_valueChanged(int value) {
  camera->setExposure(slider2value_log(value, exposurerange));
}

void MainWindow::on_gainSlider_valueChanged(int value) {
  camera->setGain(slider2value(value, gainrange));
}

void MainWindow::on_exposureAutoButton_toggled(bool checked) {
  exposureSlider->setEnabled(!checked);
  exposureSpinbox->setEnabled(!checked);
  camera->setAutoExposure(checked);
}

void MainWindow::on_gainAutoButton_toggled(bool checked) {
  gainSlider->setEnabled(!checked);
  gainSpinbox->setEnabled(!checked);
  camera->setAutoGain(checked);
}

void MainWindow::on_pixelFormatSelector_currentIndexChanged(int index) {
  auto format = pixelFormatSelector->itemData(index).toString();
  camera->setPixelFormat(format);
}

void MainWindow::on_applyROIButton_clicked(bool clicked) {
  QRect ROI(xSpinbox->value(), ySpinbox->value(),
            wSpinbox->value(), hSpinbox->value());
  if (roirange.contains(ROI)) {
    bool tostart = started;
    startVideo(false);
    camera->setROI(ROI);
    QRect roi = camera->getROI();
    xSpinbox->setValue(roi.x());
    ySpinbox->setValue(roi.y());
    wSpinbox->setValue(roi.width());
    hSpinbox->setValue(roi.height());
    startVideo(tostart);
  }
}

void MainWindow::on_resetROIButton_clicked(bool clicked) {
  bool tostart = started;
  startVideo(false);
  camera->setROI(camera->getROIMaxSize());
  // It needs to be applied twice to reach maximum size.
  camera->setROI(camera->getROIMaxSize());
  readROILimits();
  QRect roi = camera->getROI();
  xSpinbox->setValue(roi.x());
  ySpinbox->setValue(roi.y());
  wSpinbox->setValue(roi.width());
  hSpinbox->setValue(roi.height());
  startVideo(tostart);
}

void MainWindow::on_binSpinBox_valueChanged(int value) {
  bool tostart = started;
  startVideo(false);
  int bin = binSpinBox->value();
  camera->setBinning(QSize(bin, bin));
  QSize binsize = camera->getBinning();
  binSpinBox->blockSignals(true);
  binSpinBox->setValue(binsize.width());
  binSpinBox->blockSignals(false);
  readROILimits();
  auto roi = camera->getROI();
  xSpinbox->setValue(roi.x());
  ySpinbox->setValue(roi.y());
  wSpinbox->setValue(roi.width());
  hSpinbox->setValue(roi.height());
  startVideo(tostart);
}

static QVector<QRgb> initHighlightColors() {
  QVector<QRgb> colors(2);
  colors[0] = qRgba(255, 0, 200, 0);
  colors[1] = qRgba(255, 0, 200, 255);
  return colors;
}

static QVector<QRgb> highlightColors = initHighlightColors();

void MainWindow::takeNextFrame() {
  if (playing || recording) {
    QByteArray frame = camera->getFrame(dropInvalidFrames->isChecked());
    if (!frame.isEmpty()) {
      framecounter++;
      currentFrame = frame;
    }
    if (playing) {
      QImage img;
      if (frame.isEmpty()) img = invalidImage;
      else img = decoder->decode(frame);

      if (imageTransform.type() != QTransform::TxNone)
        img = img.transformed(imageTransform);
      if (invertColors->isChecked()) img.invertPixels();

      if (markClipped->isChecked()) {
        if (img.format() == QImage::Format_Indexed8) {
          auto colors = img.colorTable();
          colors[255] = qRgb(255, 0, 200);
          img.setColorTable(colors);
        } else {
          auto mask = img.createMaskFromColor(qRgb(255, 255, 255));
          mask.setColorTable(highlightColors);
          QPainter painter(&img);
          painter.drawImage(img.rect(), mask);
        }
      }

      video->setImage(img);
    }
    if (recording && !frame.isEmpty()) {
      qint64 written = recordingfile->write(frame);
      if (written != frame.size())
        qDebug() << "Incomplete write!";
    }
  }
}

void MainWindow::startVideo(bool start) {
  static QList<QWidget*> toDisableWhenPlaying;
  if (toDisableWhenPlaying.isEmpty()) toDisableWhenPlaying << cameraSelector;
  if (camera != NULL) {
    if (start && !started) {
      if (decoder != NULL) delete decoder;
      decoder = makeFrameDecoder(camera->getPixelFormat(),
                                 camera->getFrameSize());
      invalidImage = QImage(camera->getFrameSize(), QImage::Format_RGB32);
      invalidImage.fill(Qt::red);
      if (decoder == NULL)
        qDebug() << "Decoder for" << camera->getPixelFormat() <<
        "doesn't exist!";
      else {
        camera->startAcquisition();
        started = true;
        foreach (auto wgt, toDisableWhenPlaying) {
          wgt->setEnabled(false);
        }
        pixelFormatSelector->setEnabled(false);
      }
    } else if (!start && started) {
      QApplication::processEvents();
      camera->stopAcquisition();
      if (decoder != NULL) delete decoder;
      decoder = NULL;
      started = false;
      foreach (auto wgt, toDisableWhenPlaying) {
        wgt->setEnabled(true);
      }
      pixelFormatSelector->setEnabled(pixelFormatSelector->count() > 1);
    }
  }
  // Enable the snapshot button if possible.
  on_snappathEdit_textChanged();
}

void MainWindow::on_playButton_clicked(bool checked) {
  playing = checked;
  startVideo(playing || recording);
  playing = checked && started;
  playButton->setChecked(playing);
  if (!playing) video->setImage();
}

void MainWindow::on_recordButton_clicked(bool checked) {
  static QList<QWidget*> toDisableWhenRecording;
  if (toDisableWhenRecording.isEmpty()) toDisableWhenRecording << fpsSpinbox <<
         xSpinbox << wSpinbox << ySpinbox << hSpinbox << applyROIButton <<
         resetROIButton << pickROIButton << binSpinBox;
  if (checked && !recordingfile->isOpen()) {
    QIODevice::OpenMode openflags =
      recordApendCheck->isChecked() ? QIODevice::Append : QIODevice::WriteOnly;
    bool open = recordingfile->open(openflags);
    if (!open) {
      recordButton->setChecked(false);
      recordButton->setEnabled(false);
      return;
    }
  }
  recording = checked;
  startVideo(recording || playing);
  //qDebug() << "started checked" << started << checked;
  recording = checked && started;
  recordButton->setChecked(recording);
  foreach (auto wgt, toDisableWhenRecording) {
    wgt->setEnabled(!recording);
  }
  recordingTab->setEnabled(!recording);
  //qDebug() << "checked started recording:" << checked << started << recording;
}

void MainWindow::on_snapButton_clicked(bool checked) {
  if (snapshotPNG->isChecked()) {
    auto img = video->getImage();
    img.save(snappathEdit->text() + "/"
             + QString::number(QDateTime::currentMSecsSinceEpoch())
             + ".png");
  } else if (snapshotRaw->isChecked()) {
    QFile file(snappathEdit->text() + "/"
               + QString::number(QDateTime::currentMSecsSinceEpoch())
               + ".frame");
    if (file.open(QIODevice::WriteOnly)) file.write(currentFrame);
  }
}

void MainWindow::on_filenameEdit_textChanged(QString name) {
  recordButton->setEnabled(true);
  delete recordingfile;
  recordingfile = new QFile(name, this);
  auto info = QFileInfo(*recordingfile);
  auto dir = info.dir();
  recordButton->setEnabled(dir.exists());
  qDebug() << "Recording file reopened.";
}


void MainWindow::on_snappathEdit_textChanged() {
  QString name = snappathEdit->text();
  QDir dir(name);
  snapButton->setEnabled(!name.isEmpty() && dir.exists() && started);
}

void MainWindow::on_chooseFilenameButton_clicked(bool checked) {
  auto name = QFileDialog::getSaveFileName(this, "Open file");
  if(!name.isNull()) filenameEdit->setText(name);
}

void MainWindow::on_chooseSnappathButton_clicked(bool checked) {
  auto name = QFileDialog::getExistingDirectory(this, "Choose directory");
  if(!name.isNull()) snappathEdit->setText(name);
}

void MainWindow::on_fpsSpinbox_valueChanged(int value) {
  camera->setFPS(value);
  fpsSpinbox->setValue(camera->getFPS());
}

void MainWindow::pickedROI(QRect roi) {
  pickROIButton->setChecked(false);
  QRect current = camera->getROI();
  xSpinbox->setValue(current.x() + roi.x());
  ySpinbox->setValue(current.y() + roi.y());
  wSpinbox->setValue(roi.width());
  hSpinbox->setValue(roi.height());
}

void MainWindow::on_dumpSettingsButton_clicked(bool checked) {
  QFileInfo fle(filenameEdit->text());
  auto name = QFileDialog::getSaveFileName(this, "Open file", fle.dir().dirName());
  QFile outfile(name);
  bool open = outfile.open(QIODevice::WriteOnly);
  if (open) {
    QTextStream file(&outfile);
    auto id = camera->getId();
    file << "Camera: " << id.id << endl
         << "Vendor: " << id.vendor << endl
         << "Model: " << id.model << endl
         << "Pixel format: " << pixelFormatSelector->currentText() << endl
         << "FPS: " << fpsSpinbox->value() << endl
         << "Region of interest: "
         << xSpinbox->value() << "+"
         << ySpinbox->value() << "+"
         << wSpinbox->value() << "x"
         << hSpinbox->value() << endl
         << "Binning: " << binSpinBox->value() << endl
         << "Gain: " << gainSpinbox->value();
    file << (gainAutoButton->isChecked() ? " (Auto)" : " (Manual)");
    file << endl
         << "Exposure: " << exposureSpinbox->value();
    file << (exposureAutoButton->isChecked() ? " (Auto)" : " (Manual)");
    file << endl;
    outfile.close();
  }
}

void MainWindow::updateBandwidthEstimation() {
  int bw = camera->getEstimatedBW();
  if (bw == 0) {
    bandwidthDescription->setText("Not an ethernet camera.");
  } else {
    QString unit(" B/s");
    if (bw >= 1024) {
      bw /= 1024;
      unit = " kB/s";
    }
    if (bw >= 1024) {
      bw /= 1024;
      unit = " MB/s";
    }
    bandwidthDescription->setText(QString::number(bw) + unit);
  }
}

void MainWindow::closeEvent(QCloseEvent *event) {
  QSettings settings;
  settings.setValue("mainwindow/geometry", saveGeometry());
  settings.setValue("mainwindow/state", saveState());
  QMainWindow::closeEvent(event);
}

void MainWindow::updateImageTransform() {
  imageTransform.reset();
  imageTransform.scale(flipHorizontal->isChecked() ? -1 : 1,
                       flipVertical->isChecked() ? -1 : 1);
  int angle = rotationSelector->itemData(rotationSelector->
                                         currentIndex()).toInt();
  imageTransform.rotate(angle);
}

void MainWindow::showFPS() {
  actualFPS->setValue(framecounter);
  framecounter = 0;
}

void MainWindow::on_editExposureButton_clicked(bool checked) {
  autoreadexposure->stop();
  exposureSpinbox->setReadOnly(false);
  exposureSpinbox->setFocus(Qt::OtherFocusReason);
  exposureSpinbox->selectAll();
}

void MainWindow::on_editGainButton_clicked(bool checked) {
  autoreadexposure->stop();
  gainSpinbox->setReadOnly(false);
  gainSpinbox->setFocus(Qt::OtherFocusReason);
  gainSpinbox->selectAll();
}

void MainWindow::on_gainSpinbox_editingFinished() {
  camera->setGain(gainSpinbox->value());
  gainSpinbox->setReadOnly(true);
  readGain();
  autoreadexposure->start();
}

void MainWindow::on_exposureSpinbox_editingFinished() {
  camera->setExposure(exposureSpinbox->value());
  gainSpinbox->setReadOnly(true);
  readExposure();
  autoreadexposure->start();
}

void MainWindow::on_videodock_visibilityChanged(bool visible) {
  showVideoButton->blockSignals(true);
  showVideoButton->setChecked(visible);
  showVideoButton->blockSignals(false);
}
