/*  Ekos Alignment Module
    Copyright (C) 2013 Jasem Mutlaq <mutlaqja@ikarustech.com>

    This application is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.
 */

#include <config-kstars.h>
#include <QProcess>

#include "kstars.h"
#include "kstarsdata.h"
#include "align.h"
#include "dms.h"
#include "Options.h"

#include <KMessageBox>

#include "QProgressIndicator.h"
#include "indi/driverinfo.h"
#include "indi/indicommon.h"

#include "fitsviewer/fitsviewer.h"
#include "fitsviewer/fitstab.h"
#include "fitsviewer/fitsview.h"

#ifdef HAVE_QJSON
#include "onlineastrometryparser.h"
#endif
#include "offlineastrometryparser.h"

#include <basedevice.h>

namespace Ekos
{

// 30 arcmiutes RA movement
const double Align::RAMotion = 0.5;
// Sidereal rate, degrees/s
const float Align::SIDRATE  = 0.004178;

Align::Align()
{
    setupUi(this);

    currentCCD     = NULL;
    currentTelescope = NULL;
    parser = NULL;
    #ifdef HAVE_QJSON
    onlineParser = NULL;
    #endif
    offlineParser = NULL;
    ccd_hor_pixel =  ccd_ver_pixel =  focal_length =  aperture = -1;
    decDeviation = azDeviation = altDeviation = 0;
    useGuideHead = false;
    canSync = false;

    connect(solveB, SIGNAL(clicked()), this, SLOT(capture()));
    connect(stopB, SIGNAL(clicked()), this, SLOT(stopSolving()));
    connect(measureAltB, SIGNAL(clicked()), this, SLOT(measureAltError()));
    connect(measureAzB, SIGNAL(clicked()), this, SLOT(measureAzError()));
    connect(polarR, SIGNAL(toggled(bool)), this, SLOT(checkPolarAlignment()));
    connect(raBox, SIGNAL(textChanged( const QString & ) ), this, SLOT( checkLineEdits() ) );
    connect(decBox, SIGNAL(textChanged( const QString & ) ), this, SLOT( checkLineEdits() ) );
    connect(syncBoxesB, SIGNAL(clicked()), this, SLOT(copyCoordsToBoxes()));
    connect(clearBoxesB, SIGNAL(clicked()), this, SLOT(clearCoordBoxes()));
    connect(CCDCaptureCombo, SIGNAL(activated(int)), this, SLOT(checkCCD(int)));
    connect(correctAltB, SIGNAL(clicked()), this, SLOT(correctAltError()));
    connect(correctAzB, SIGNAL(clicked()), this, SLOT(correctAzError()));

    kcfg_solverXBin->setValue(Options::solverXBin());
    kcfg_solverYBin->setValue(Options::solverYBin());
    kcfg_solverUpdateCoords->setChecked(Options::solverUpdateCoords());

    syncBoxesB->setIcon(KIcon("edit-copy"));
    clearBoxesB->setIcon(KIcon("edit-clear"));

    raBox->setDegType(false); //RA box should be HMS-style

    appendLogText(i18n("Idle."));

    pi = new QProgressIndicator(this);

    controlLayout->addWidget(pi, 0, 2, 1, 1);

    exposureSpin->setValue(Options::alignExposure());

    altStage = ALT_INIT;
    azStage  = AZ_INIT;

    #ifndef HAVE_QJSON
    kcfg_onlineSolver->setChecked(false);
    kcfg_onlineSolver->setEnabled(false);
    #else
    kcfg_onlineSolver->setChecked(Options::solverOnline());
    #endif

    kcfg_offlineSolver->setChecked(Options::solverOnline() == false);
    connect(kcfg_onlineSolver, SIGNAL(toggled(bool)), SLOT(updateSolverType(bool)));


    if (kcfg_onlineSolver->isChecked())
    {
        #ifdef HAVE_QJSON
        onlineParser = new Ekos::OnlineAstrometryParser();
        parser = onlineParser;
        #endif
    }
    else
    {
        offlineParser = new OfflineAstrometryParser();
        parser = offlineParser;
    }

    parser->setAlign(this);
    if (parser->init() == false)
        setEnabled(false);
    else
    {
        connect(parser, SIGNAL(solverFinished(double,double,double)), this, SLOT(solverFinished(double,double,double)));
        connect(parser, SIGNAL(solverFailed()), this, SLOT(solverFailed()));
    }

}

Align::~Align()
{
    delete(pi);
}

bool Align::parserOK()
{
    bool rc = parser->init();

    if (rc)
    {
        connect(parser, SIGNAL(solverFinished(double,double,double)), this, SLOT(solverFinished(double,double,double)));
        connect(parser, SIGNAL(solverFailed()), this, SLOT(solverFailed()));
    }

    return rc;
}

bool Align::isVerbose()
{
    return solverVerbose->isChecked();
}

void Align::updateSolverType(bool useOnline)
{

    if (useOnline)
    {
        #ifdef HAVE_QJSON
        if (onlineParser != NULL)
        {
            parser = onlineParser;
            return;
        }

        onlineParser = new Ekos::OnlineAstrometryParser();
        parser = onlineParser;
        #endif
    }
    else
    {
        if (offlineParser != NULL)
        {
            parser = offlineParser;
            return;
        }

        offlineParser = new Ekos::OfflineAstrometryParser();
        parser = offlineParser;
    }

    parser->setAlign(this);
    if (parser->init())
    {
        connect(parser, SIGNAL(solverFinished(double,double,double)), this, SLOT(solverFinished(double,double,double)));
        connect(parser, SIGNAL(solverFailed()), this, SLOT(solverFailed()));
    }
    else
        parser->disconnect();

}

void Align::checkCCD(int ccdNum)
{
    if (ccdNum <= CCDs.count())
        currentCCD = CCDs.at(ccdNum);

    syncCCDInfo();

}

void Align::addCCD(ISD::GDInterface *newCCD)
{
    CCDCaptureCombo->addItem(newCCD->getDeviceName());

    CCDs.append(static_cast<ISD::CCD *>(newCCD));

    checkCCD(0);

    CCDCaptureCombo->setCurrentIndex(0);
}

void Align::setTelescope(ISD::GDInterface *newTelescope)
{
    currentTelescope = static_cast<ISD::Telescope*> (newTelescope);

    connect(currentTelescope, SIGNAL(numberUpdated(INumberVectorProperty*)), this, SLOT(updateScopeCoords(INumberVectorProperty*)));

    syncTelescopeInfo();
}

void Align::syncTelescopeInfo()
{
    INumberVectorProperty * nvp = currentTelescope->getBaseDevice()->getNumber("TELESCOPE_INFO");

    if (nvp)
    {
        INumber *np = IUFindNumber(nvp, "TELESCOPE_APERTURE");

        if (np && np->value > 0)
            aperture = np->value;
        else
        {
            np = IUFindNumber(nvp, "GUIDER_APERTURE");
            if (np && np->value > 0)
                aperture = np->value;
        }

        np = IUFindNumber(nvp, "TELESCOPE_FOCAL_LENGTH");
        if (np && np->value > 0)
            focal_length = np->value;
        else
        {
            np = IUFindNumber(nvp, "GUIDER_FOCAL_LENGTH");
            if (np && np->value > 0)
                focal_length = np->value;
        }
    }

    if (focal_length == -1 || aperture == -1)
        return;

    if (ccd_hor_pixel != -1 && ccd_ver_pixel != -1 && focal_length != -1 && aperture != -1)
        calculateFOV();

    if (currentCCD && currentTelescope)
        generateArgs();

    if (syncR->isEnabled() && (canSync = currentTelescope->canSync()) == false)
    {
        syncR->setEnabled(false);
        slewR->setChecked(true);
        appendLogText(i18n("Telescope does not support syncing."));
    }
}


void Align::syncCCDInfo()
{
    INumberVectorProperty * nvp = NULL;
    int x,y;

    if (currentCCD == NULL)
        return;

    if (useGuideHead)
        nvp = currentCCD->getBaseDevice()->getNumber("GUIDER_INFO");
    else
        nvp = currentCCD->getBaseDevice()->getNumber("CCD_INFO");

    if (nvp)
    {
        INumber *np = IUFindNumber(nvp, "CCD_PIXEL_SIZE_X");
        if (np && np->value >0)
            ccd_hor_pixel = ccd_ver_pixel = np->value;

        np = IUFindNumber(nvp, "CCD_PIXEL_SIZE_Y");
        if (np && np->value >0)
            ccd_ver_pixel = np->value;

        np = IUFindNumber(nvp, "CCD_PIXEL_SIZE_Y");
        if (np && np->value >0)
            ccd_ver_pixel = np->value;
    }    

    ISD::CCDChip *targetChip = currentCCD->getChip(useGuideHead ? ISD::CCDChip::GUIDE_CCD : ISD::CCDChip::PRIMARY_CCD);

    targetChip->getFrame(&x,&y,&ccd_width,&ccd_height);
    kcfg_solverXBin->setEnabled(targetChip->canBin());
    kcfg_solverYBin->setEnabled(targetChip->canBin());

    if (ccd_hor_pixel == -1 || ccd_ver_pixel == -1)
        return;

    if (ccd_hor_pixel != -1 && ccd_ver_pixel != -1 && focal_length != -1 && aperture != -1)    
        calculateFOV();

    if (currentCCD && currentTelescope)
        generateArgs();

}


void Align::calculateFOV()
{
    // Calculate FOV
    fov_x = 206264.8062470963552 * ccd_width * ccd_hor_pixel / 1000.0 / focal_length;
    fov_y = 206264.8062470963552 * ccd_height * ccd_ver_pixel / 1000.0 / focal_length;

    fov_x /= 60.0;
    fov_y /= 60.0;

    FOVOut->setText(QString("%1' x %2'").arg(fov_x, 0, 'g', 3).arg(fov_y, 0, 'g', 3));

    parser->verifyIndexFiles(fov_x, fov_y);

}

void Align::generateArgs()
{
    // -O overwrite
    // -3 Expected RA
    // -4 Expected DEC
    // -5 Radius (deg)
    // -L lower scale of image in arcminutes
    // -H upper scale of image in arcmiutes
    // -u aw set scale to be in arcminutes
    // -W solution.wcs name of solution file
    // apog1.jpg name of target file to analyze
    //solve-field -O -3 06:40:51 -4 +09:49:53 -5 1 -L 40 -H 100 -u aw -W solution.wcs apod1.jpg

    double ra=0,dec=0, fov_lower, fov_upper;
    QString ra_dms, dec_dms;
    QString fov_low,fov_high;
    QStringList solver_args;

    // let's stretch the boundaries by 5%
    fov_lower = ((fov_x < fov_y) ? (fov_x *0.95) : (fov_y *0.95));
    fov_upper = ((fov_x > fov_y) ? (fov_x * 1.05) : (fov_y * 1.05));

    currentTelescope->getEqCoords(&ra, &dec);

    fov_low  = QString("%1").arg(fov_lower);
    fov_high = QString("%1").arg(fov_upper);

    getFormattedCoords(ra, dec, ra_dms, dec_dms);

    solver_args << "--no-verify" << "--no-plots" << "--no-fits2fits" << "--resort"
                << "-O" << "-L" << fov_low << "-H" << fov_high << "-u" << "aw";

    if (raBox->isEmpty() == false && decBox->isEmpty() == false)
    {
        bool raOk(false), decOk(false), radiusOk(false);
        dms ra( raBox->createDms( false, &raOk ) ); //false means expressed in hours
        dms dec( decBox->createDms( true, &decOk ) );
        int radius = 30;
        QString message;

        if ( raOk && decOk )
        {
            //make sure values are in valid range
            if ( ra.Hours() < 0.0 || ra.Hours() > 24.0 )
                message = i18n( "The Right Ascension value must be between 0.0 and 24.0." );
            if ( dec.Degrees() < -90.0 || dec.Degrees() > 90.0 )
                message += '\n' + i18n( "The Declination value must be between -90.0 and 90.0." );
            if ( ! message.isEmpty() )
            {
                KMessageBox::sorry( 0, message, i18n( "Invalid Coordinate Data" ) );
                return;
            }
        }

        if (radiusBox->text().isEmpty() == false)
            radius = radiusBox->text().toInt(&radiusOk);

        if (radiusOk == false)
        {
            KMessageBox::sorry( 0, message, i18n( "Invalid radius value" ) );
            return;
        }

        solver_args << "-3" << QString().setNum(ra.Degrees()) << "-4" << QString().setNum(dec.Degrees()) << "-5" << QString().setNum(radius);
     }

    solverOptions->setText(solver_args.join(" "));
}

void Align::checkLineEdits()
{
   bool raOk(false), decOk(false);
   raBox->createDms( false, &raOk );
   decBox->createDms( true, &decOk );
   if ( raOk && decOk )
            generateArgs();
}

void Align::copyCoordsToBoxes()
{
    raBox->setText(ScopeRAOut->text());
    decBox->setText(ScopeDecOut->text());

    checkLineEdits();
}

void Align::clearCoordBoxes()
{
    raBox->clear();
    decBox->clear();

    generateArgs();
}

bool Align::capture()
{
    if (currentCCD == NULL)
        return false;

    if (parser->init() == false)
        return false;

    if (focal_length == -1 || aperture == -1)
    {
        KMessageBox::error(0, i18n("Telescope aperture and focal length are missing. Please check your driver settings and try again."));
        return false;
    }

    if (ccd_hor_pixel == -1 || ccd_ver_pixel == -1)
    {
        KMessageBox::error(0, i18n("CCD pixel size is missing. Please check your driver settings and try again."));
        return false;
    }

    double seqExpose = exposureSpin->value();

    ISD::CCDChip *targetChip = currentCCD->getChip(useGuideHead ? ISD::CCDChip::GUIDE_CCD : ISD::CCDChip::PRIMARY_CCD);

    CCDFrameType ccdFrame = FRAME_LIGHT;

    if (currentCCD->isConnected() == false)
    {
        appendLogText(i18n("Error: Lost connection to CCD."));
        return false;
    }

   connect(currentCCD, SIGNAL(BLOBUpdated(IBLOB*)), this, SLOT(newFITS(IBLOB*)));

   targetChip->setCaptureMode(FITS_WCSM);
   targetChip->setBinning(kcfg_solverXBin->value(), kcfg_solverYBin->value());
   targetChip->setFrameType(ccdFrame);

   targetChip->capture(seqExpose);

   Options::setAlignExposure(seqExpose);

   solveB->setEnabled(false);
   stopB->setEnabled(true);
   pi->startAnimation();

   appendLogText(i18n("Capturing image..."));

   return true;
}

void Align::newFITS(IBLOB *bp)
{
    // Ignore guide head if there is any.
    if (!strcmp(bp->name, "CCD2"))
        return;

    currentCCD->disconnect(this);

    appendLogText(i18n("Image received."));

    startSovling(QString(bp->label));
}

void Align::startSovling(const QString &filename)
{
    QStringList solverArgs;
    double ra,dec;

    Options::setSolverXBin(kcfg_solverXBin->value());
    Options::setSolverYBin(kcfg_solverYBin->value());
    Options::setSolverUpdateCoords(kcfg_solverUpdateCoords->isChecked());
    Options::setSolverOnline(kcfg_onlineSolver->isChecked());

    currentTelescope->getEqCoords(&ra, &dec);

    targetCoord.setRA(ra);
    targetCoord.setDec(dec);

    solverTimer.start();

    solverArgs = solverOptions->text().split(" ");

    parser->startSovler(filename, solverArgs);

}

void Align::solverFinished(double orientation, double ra, double dec)
{
    pi->stopAnimation();
    stopB->setEnabled(false);
    solveB->setEnabled(true);

     alignCoord.setRA0(ra/15.0);
     alignCoord.setDec0(dec);
     RotOut->setText(QString("%1").arg(orientation, 0, 'g', 5));

     // Convert to JNow
     alignCoord.apparentCoord((long double) J2000, KStars::Instance()->data()->ut().djd());

     QString ra_dms, dec_dms;
     getFormattedCoords(alignCoord.ra().Hours(), alignCoord.dec().Degrees(), ra_dms, dec_dms);

     SolverRAOut->setText(ra_dms);
     SolverDecOut->setText(dec_dms);

     executeMode();

}

void Align::solverFailed()
{
    pi->stopAnimation();
    stopB->setEnabled(false);
    solveB->setEnabled(true);

    azStage  = AZ_INIT;
    altStage = ALT_INIT;


}

void Align::stopSolving()
{
    parser->stopSolver();
    pi->stopAnimation();
    stopB->setEnabled(false);
    solveB->setEnabled(true);

    azStage  = AZ_INIT;
    altStage = ALT_INIT;

    ISD::CCDChip *targetChip = currentCCD->getChip(useGuideHead ? ISD::CCDChip::GUIDE_CCD : ISD::CCDChip::PRIMARY_CCD);

    // If capture is still in progress, let's stop that.
    if (targetChip->isCapturing())
    {
        targetChip->abortExposure();
        appendLogText(i18n("Capture aborted."));
    }
    else
    {
        int elapsed = (int) round(solverTimer.elapsed()/1000.0);
        appendLogText(i18np("Solver aborted after %1 second.", "Solver aborted after %1 seconds", elapsed));
    }
}

void Align::appendLogText(const QString &text)
{
    logText.insert(0, i18nc("log entry; %1 is the date, %2 is the text", "%1 %2", QDateTime::currentDateTime().toString("yyyy-MM-ddThh:mm:ss"), text));

    emit newLog();
}

void Align::clearLog()
{
    logText.clear();
    emit newLog();
}

void Align::updateScopeCoords(INumberVectorProperty *coord)
{
    QString ra_dms, dec_dms;
    static bool slew_dirty=false;

    if (!strcmp(coord->name, "EQUATORIAL_EOD_COORD"))
    {        
        getFormattedCoords(coord->np[0].value, coord->np[1].value, ra_dms, dec_dms);

        telescopeCoord.setRA(coord->np[0].value);
        telescopeCoord.setDec(coord->np[1].value);

        ScopeRAOut->setText(ra_dms);
        ScopeDecOut->setText(dec_dms);

        if (kcfg_solverUpdateCoords->isChecked())
        {
            if (currentTelescope->isSlewing() && slew_dirty == false)
                slew_dirty = true;
            else if (currentTelescope->isSlewing() == false && slew_dirty)
            {
                slew_dirty = false;
                copyCoordsToBoxes();
            }
        }

        switch (azStage)
        {
             case AZ_SYNCING:
            if (currentTelescope->isSlewing())
                azStage=AZ_SLEWING;
                break;

            case AZ_SLEWING:
            if (currentTelescope->isSlewing() == false)
            {
                azStage = AZ_SECOND_TARGET;
                measureAzError();
            }
            break;

        case AZ_CORRECTING:
         if (currentTelescope->isSlewing() == false)
         {
             if(decDeviation > 0){
                 appendLogText(i18n("Slew complete. Please adjust your mount's' azimuth knob eastward until the target is in the center of the view."));
             }
             else{
                 appendLogText(i18n("Slew complete. Please adjust your mount's' azimuth knob westward until the target is in the center of the view."));
             }
             azStage = AZ_INIT;
         }
         break;

           default:
            break;
        }

        switch (altStage)
        {
           case ALT_SYNCING:
            if (currentTelescope->isSlewing())
                altStage = ALT_SLEWING;
                break;

           case ALT_SLEWING:
            if (currentTelescope->isSlewing() == false)
            {
                altStage = ALT_SECOND_TARGET;
                measureAltError();
            }
            break;

           case ALT_CORRECTING:
            if (currentTelescope->isSlewing() == false)
            {
                if(decDeviation > 0){
                    appendLogText(i18n("Slew complete. Please lower the altitude knob on your mount until the target is in the center of the view."));
                }
                else{
                    appendLogText(i18n("Slew complete. Please raise the altitude knob on your mount until the target is in the center of the view."));
                }
                altStage = ALT_INIT;
            }
            break;


           default:
            break;
        }
    }

    if (!strcmp(coord->name, "TELESCOPE_INFO"))
        syncTelescopeInfo();

}

void Align::executeMode()
{
    if (gotoR->isChecked())
        executeGOTO();
    else
        executePolarAlign();
}


void Align::executeGOTO()
{
    if (syncR->isChecked())
        Sync();
    else if (slewR->isChecked())
        SlewToTarget();
}

void Align::Sync()
{
    if (currentTelescope->Sync(&alignCoord))
        appendLogText(i18n("Syncing successful."));
    else
        appendLogText(i18n("Syncing failed."));

}

void Align::SlewToTarget()
{
    if (canSync)
        Sync();

    currentTelescope->Slew(&targetCoord);

    appendLogText(i18n("Slewing to target."));
}

void Align::checkPolarAlignment()
{
    if (polarR->isChecked())
    {
        measureAltB->setEnabled(true);
        measureAzB->setEnabled(true);
        gotoBox->setEnabled(false);
    }
    else
    {
        measureAltB->setEnabled(false);
        measureAzB->setEnabled(false);
        gotoBox->setEnabled(true);
    }
}

void Align::executePolarAlign()
{
    appendLogText(i18n("Processing solution for polar alignment..."));

    switch (azStage)
    {
        case AZ_FIRST_TARGET:
        case AZ_FINISHED:
            measureAzError();
            break;

        default:
            break;
    }

    switch (altStage)
    {
        case ALT_FIRST_TARGET:
        case ALT_FINISHED:
            measureAltError();
            break;

        default:
            break;
    }
}


void Align::measureAzError()
{
    static double initRA=0, initDEC=0, finalRA=0, finalDEC=0;
    QString meridianOrient = (hemisphereCombo->currentIndex() == 0) ? i18n("southern") : i18n("northern");

    switch (azStage)
    {
        case AZ_INIT:

        // Display message box confirming user point scope near meridian and south

        if (KMessageBox::warningContinueCancel( 0, i18n("Point the telescope at the %1 meridian. Press continue when ready.", meridianOrient)
                                                , i18n("Polar Alignment Measurement"), KStandardGuiItem::cont(), KStandardGuiItem::cancel(),
                                                i18n("Don't ask again"))!=KMessageBox::Continue)
            return;

        appendLogText(i18n("Solving first frame near the meridian."));
        azStage = AZ_FIRST_TARGET;
        polarR->setChecked(true);
        solveB->click();
        break;

      case AZ_FIRST_TARGET:
        // start solving there, find RA/DEC
        initRA   = alignCoord.ra().Degrees();
        initDEC  = alignCoord.dec().Degrees();

        // Now move 30 arcminutes in RA
        if (canSync)
        {
            azStage = AZ_SYNCING;
            currentTelescope->Sync(initRA/15.0, initDEC);
            currentTelescope->Slew((initRA - RAMotion)/15.0, initDEC);
        }
        // If telescope doesn't sync, we slew relative to its current coordinates
        else
        {
            azStage = AZ_SLEWING;
            currentTelescope->Slew(telescopeCoord.ra().Hours() - RAMotion/15.0, telescopeCoord.dec().Degrees());
        }

        appendLogText(i18n("Slewing 30 arcminutes in RA..."));
        break;

      case AZ_SECOND_TARGET:
        // We reached second target now
        // Let now solver for RA/DEC
        appendLogText(i18n("Solving second frame near the meridian."));
        azStage = AZ_FINISHED;
        polarR->setChecked(true);
        solveB->click();
        break;


      case AZ_FINISHED:
        // Measure deviation in DEC
        // Call function to report error
        // set stage to AZ_FIRST_TARGET again
        appendLogText(i18n("Calculating azimuth alignment error..."));
        finalRA   = alignCoord.ra().Degrees();
        finalDEC  = alignCoord.dec().Degrees();

        // Slew back to original position
        if (canSync)
            currentTelescope->Slew(initRA/15.0, initDEC);
        else
        {
            currentTelescope->Slew(telescopeCoord.ra().Hours() + RAMotion/15.0, telescopeCoord.dec().Degrees());
        }

        appendLogText(i18n("Slewing back to original position..."));

        calculatePolarError(initRA, initDEC, finalRA, finalDEC);

        azStage = AZ_INIT;
        break;

    default:
        break;

    }

}

void Align::measureAltError()
{
    static double initRA=0, initDEC=0, finalRA=0, finalDEC=0;
    QString meridianOrient = (altDirCombo->currentIndex() == 0) ? i18n("east") : i18n("west");

    switch (altStage)
    {
        case ALT_INIT:

        // Display message box confirming user point scope near meridian and south

        if (KMessageBox::warningContinueCancel( 0, i18n("Point the telescope to the %1 with a minimum altitude of 20 degrees. Press continue when ready.", meridianOrient)
                                                , i18n("Polar Alignment Measurement"), KStandardGuiItem::cont(), KStandardGuiItem::cancel(),
                                                i18n("Don't ask again"))!=KMessageBox::Continue)
            return;

        appendLogText(i18n("Solving first frame."));
        altStage = ALT_FIRST_TARGET;
        polarR->setChecked(true);
        solveB->click();
        break;

      case ALT_FIRST_TARGET:
        // start solving there, find RA/DEC
        initRA   = alignCoord.ra().Degrees();
        initDEC  = alignCoord.dec().Degrees();

        // Now move 30 arcminutes in RA
        if (canSync)
        {
            altStage = ALT_SYNCING;
            currentTelescope->Sync(initRA/15.0, initDEC);
            currentTelescope->Slew((initRA - RAMotion)/15.0, initDEC);

        }
        // If telescope doesn't sync, we slew relative to its current coordinates
        else
        {
            altStage = ALT_SLEWING;
            currentTelescope->Slew(telescopeCoord.ra().Hours() - RAMotion/15.0, telescopeCoord.dec().Degrees());
        }


        appendLogText(i18n("Slewing 30 arcminutes in RA..."));
        break;

      case ALT_SECOND_TARGET:
        // We reached second target now
        // Let now solver for RA/DEC
        appendLogText(i18n("Solving second frame."));
        altStage = ALT_FINISHED;
        polarR->setChecked(true);
        solveB->click();
        break;


      case ALT_FINISHED:
        // Measure deviation in DEC
        // Call function to report error
        appendLogText(i18n("Calculating altitude alignment error..."));
        finalRA   = alignCoord.ra().Degrees();
        finalDEC  = alignCoord.dec().Degrees();

        // Slew back to original position
        if (canSync)
            currentTelescope->Slew(initRA/15.0, initDEC);
        // If telescope doesn't sync, we slew relative to its current coordinates
        else
        {
            currentTelescope->Slew(telescopeCoord.ra().Hours() + RAMotion/15.0, telescopeCoord.dec().Degrees());
        }

        appendLogText(i18n("Slewing back to original position..."));

        calculatePolarError(initRA, initDEC, finalRA, finalDEC);

        altStage = ALT_INIT;
        break;

    default:
        break;

    }

}

void Align::calculatePolarError(double initRA, double initDEC, double finalRA, double finalDEC)
{
    double raMotion = finalRA - initRA;
    decDeviation = finalDEC - initDEC;

    // How much time passed siderrally form initRA to finalRA?
    double RATime = fabs(raMotion / SIDRATE) / 60.0;

    qDebug() << "initRA " << initRA << " initDEC " << initDEC << " finalRA " << finalRA << " finalDEC " << finalDEC << endl;
    qDebug() << "decDeviation " << decDeviation*3600 << " arcsec " << " RATime " << RATime << endl;

    // Equation by Frank Berret (Measuring Polar Axis Alignment Error, page 4)
    // In degrees
    double deviation = (3.81 * (decDeviation * 3600) ) / ( RATime * cos(initDEC * dms::DegToRad)) / 60.0;

    QString deviationDirection;

    switch (hemisphereCombo->currentIndex())
    {
        // Northern hemisphere
        case 0:
        if (azStage == AZ_FINISHED)
        {
            if (decDeviation > 0)
                deviationDirection = "too far west";
            else
                deviationDirection = "too far east";
        }
        else if (altStage == ALT_FINISHED)
        {
            switch (altDirCombo->currentIndex())
            {
                // East
                case 0:
                if (decDeviation > 0)
                    deviationDirection = "too far high";
                else
                    deviationDirection = "too far low";
                break;

                // West
                case 1:
                if (decDeviation > 0)
                    deviationDirection = "too far low";
                else
                    deviationDirection = "too far high";
                break;

                default:
                break;
            }
        }
        break;

        // Southern hemisphere
        case 1:
        if (azStage == AZ_FINISHED)
        {
            if (decDeviation > 0)
                deviationDirection = "too far east";
            else
                deviationDirection = "too far west";
        }
        else if (altStage == ALT_FINISHED)
        {
            switch (altDirCombo->currentIndex())
            {
                // East
                case 0:
                if (decDeviation > 0)
                    deviationDirection = "too far low";
                else
                    deviationDirection = "too far high";
                break;

                // West
                case 1:
                if (decDeviation > 0)
                    deviationDirection = "too far high";
                else
                    deviationDirection = "too far low";
                break;

                default:
                break;
            }
        }
        break;

       default:
        break;

    }

    if (azStage == AZ_FINISHED)
    {
        azError->setText(i18n("%1° %2", QString("%1").arg(fabs(deviation), 0, 'g', 3), i18n("%1", deviationDirection)));
        azDeviation = deviation * (decDeviation > 0 ? 1 : -1);
        correctAzB->setEnabled(true);
    }
    if (altStage == ALT_FINISHED)
    {
        altError->setText(i18n("%1° %2", QString("%1").arg(fabs(deviation), 0, 'g', 3), i18n("%1", deviationDirection)));
        altDeviation = deviation * (decDeviation > 0 ? 1 : -1);
        correctAltB->setEnabled(true);
    }
}

void Align::correctAltError()
{
    double newRA, newDEC, currentAlt, currentAz;

    SkyPoint currentCoord (telescopeCoord);
    dms      targetLat;

    targetLat.setD(KStars::Instance()->data()->geo()->lat()->Degrees() + altDeviation);

    currentCoord.EquatorialToHorizontal(KStars::Instance()->data()->lst(), &targetLat );

    currentAlt = currentCoord.alt().Degrees();
    currentAz  = currentCoord.az().Degrees();

    currentCoord.setAlt(currentAlt);
    currentCoord.setAz(currentAz);

    currentCoord.HorizontalToEquatorial(KStars::Instance()->data()->lst(), KStars::Instance()->data()->geo()->lat());

    newRA  = currentCoord.ra().Hours();
    newDEC = currentCoord.dec().Degrees();

    altStage = ALT_CORRECTING;

    currentTelescope->Slew(newRA, newDEC);

    appendLogText(i18n("Slewing to calibration position, please wait until telescope is finished slewing."));

}

void Align::correctAzError()
{
    double newRA, newDEC, currentAlt, currentAz;

    SkyPoint currentCoord (telescopeCoord);

    currentCoord.EquatorialToHorizontal(KStars::Instance()->data()->lst(), KStars::Instance()->data()->geo()->lat());

    currentAlt = currentCoord.alt().Degrees();
    currentAz  = currentCoord.az().Degrees() + azDeviation;

    currentCoord.setAlt(currentAlt);
    currentCoord.setAz(currentAz);

    currentCoord.HorizontalToEquatorial(KStars::Instance()->data()->lst(), KStars::Instance()->data()->geo()->lat());

    newRA  = currentCoord.ra().Hours();
    newDEC = currentCoord.dec().Degrees();

    azStage = AZ_CORRECTING;

    currentTelescope->Slew(newRA, newDEC);

    appendLogText(i18n("Slewing to calibration position, please wait until telescope is finished slewing."));

}

void Align::getFormattedCoords(double ra, double dec, QString &ra_str, QString &dec_str)
{
    dms ra_s,dec_s;
    ra_s.setH(ra);
    dec_s.setD(dec);

    ra_str = QString("%1:%2:%3").arg(ra_s.hour(), 2, 10, QChar('0')).arg(ra_s.minute(), 2, 10, QChar('0')).arg(ra_s.second(), 2, 10, QChar('0'));
    if (dec_s.Degrees() < 0)
        dec_str = QString("-%1:%2:%3").arg(abs(dec_s.degree()), 2, 10, QChar('0')).arg(abs(dec_s.arcmin()), 2, 10, QChar('0')).arg(dec_s.arcsec(), 2, 10, QChar('0'));
    else
        dec_str = QString("%1:%2:%3").arg(dec_s.degree(), 2, 10, QChar('0')).arg(dec_s.arcmin(), 2, 10, QChar('0')).arg(dec_s.arcsec(), 2, 10, QChar('0'));
}

}

#include "align.moc"
