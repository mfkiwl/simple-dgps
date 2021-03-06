/* variance by ura ephemeris -------------------------------------------------*/
static double var_uraeph(int sys, int ura)
{
    const double ura_value[]={
        2.4,3.4,4.85,6.85,9.65,13.65,24.0,48.0,96.0,192.0,384.0,768.0,1536.0,
        3072.0,6144.0
    };
    if (sys==SYS_GAL) { /* galileo sisa (ref [7] 5.1.11) */
        if (ura<= 49) return SQR(ura*0.01);
        if (ura<= 74) return SQR(0.5+(ura- 50)*0.02);
        if (ura<= 99) return SQR(1.0+(ura- 75)*0.04);
        if (ura<=125) return SQR(2.0+(ura-100)*0.16);
        return SQR(STD_GAL_NAPA);
    }
    else { /* gps ura (ref [1] 20.3.3.3.1.1) */
        return ura<0||15<ura?SQR(6144.0):SQR(ura_value[ura]);
    }
}

void latLonToEcef(double bs_lat_lon_pos[3], double ecef[3])
{
    double lat = bs_lat_lon_pos[0];
    double lon = bs_lat_lon_pos[1];
    double alt = bs_lat_lon_pos[2];
    double clat = cos(lat * D2R);
    double slat = sin(lat * D2R);
    double clon = cos(lon * D2R);
    double slon = sin(lon * D2R);

    double N = WGS84_A / sqrt(1.0 - WGS84_E * WGS84_E * slat * slat);

    ecef[0] = (N + alt) * clat * clon;
    ecef[1] = (N + alt) * clat * slon;
    ecef[2] = (N * (1.0 - WGS84_E * WGS84_E) + alt) * slat;
}
// Coverts ECEF to ENU coordinates centered at given lat, lon
void ecefToEnu(double lat, double lon, double x, double y, double z, double xr, double yr, double zr, double *e, double *n, double *u)
{
    double clat = cos(lat * D2R);
    double slat = sin(lat * D2R);
    double clon = cos(lon * D2R);
    double slon = sin(lon * D2R);
    double dx = x - xr;
    double dy = y - yr;
    double dz = z - zr;

    *e = -slon*dx  + clon*dy;
    *n = -slat*clon*dx - slat*slon*dy + clat*dz;
    *u = clat*clon*dx + clat*slon*dy + slat*dz;
}

/* broadcast ephemeris to satellite position and clock bias --------------------
* compute satellite position and clock bias with broadcast ephemeris (gps,
* galileo, qzss)
* args   : gtime_t time     I   time (gpst)
*          eph_t *eph       I   broadcast ephemeris
*          double *rs       O   satellite position (ecef) {x,y,z} (m)
*          double *dts      O   satellite clock bias (s)
*          double *var      O   satellite position and clock variance (m^2)
* return : none
* notes  : see ref [1],[7],[8]
*          satellite clock includes relativity correction without code bias
*          (tgd or bgd)
*-----------------------------------------------------------------------------*/
static void eph2pos(gtime_t time, eph_t *eph, sat_pos *sp)
{
    double tk,M,E,Ek,sinE,cosE,u,r,i,O,sin2u,cos2u,x,y,sinO,cosO,cosi,mu,omge;
    double xg,yg,zg,sino,coso;
    int n,sys,prn;
    double dts, var;


    #ifdef LOG_DECODING_MSGS
    printf("eph2pos : time=%s sat=%2d\n", time_str(time,3),eph->sat);
    #endif

    if (eph->A<=0.0) {
        sp->pos[0]=sp->pos[1]=sp->pos[2]=dts=var=0.0;
        return;
    }
    tk=timediff(time,eph->toe);

    switch ((sys=satsys(eph->sat,&prn))) {
        case SYS_GAL: mu=MU_GAL; omge=OMGE_GAL; break;
        case SYS_CMP: mu=MU_CMP; omge=OMGE_CMP; break;
        default:      mu=MU_GPS; omge=OMGE;     break;
    }
    M=eph->M0+(sqrt(mu/(eph->A*eph->A*eph->A))+eph->deln)*tk;

    for (n=0,E=M,Ek=0.0;fabs(E-Ek)>RTOL_KEPLER&&n<MAX_ITER_KEPLER;n++) {
        Ek=E; E-=(E-eph->e*sin(E)-M)/(1.0-eph->e*cos(E));
    }
    if (n>=MAX_ITER_KEPLER) {

        #ifdef LOG_DECODING_MSGS
        printf("eph2pos: kepler iteration overflow sat=%2d\n",eph->sat);
        #endif
        return;
    }
    sinE=sin(E); cosE=cos(E);

    #ifdef LOG_DECODING_MSGS
    printf("kepler: sat=%2d e=%8.5f n=%2d del=%10.3e\n",eph->sat,eph->e,n,E-Ek);
    #endif

    u=atan2(sqrt(1.0-eph->e*eph->e)*sinE,cosE-eph->e)+eph->omg;
    r=eph->A*(1.0-eph->e*cosE);
    i=eph->i0+eph->idot*tk;
    sin2u=sin(2.0*u); cos2u=cos(2.0*u);
    u+=eph->cus*sin2u+eph->cuc*cos2u;
    r+=eph->crs*sin2u+eph->crc*cos2u;
    i+=eph->cis*sin2u+eph->cic*cos2u;
    x=r*cos(u); y=r*sin(u); cosi=cos(i);

    /* beidou geo satellite */
    if (sys==SYS_CMP&&(eph->flag==2||(eph->flag==0&&prn<=5))) {
        O=eph->OMG0+eph->OMGd*tk-omge*eph->toes;
        sinO=sin(O); cosO=cos(O);
        xg=x*cosO-y*cosi*sinO;
        yg=x*sinO+y*cosi*cosO;
        zg=y*sin(i);
        sino=sin(omge*tk); coso=cos(omge*tk);
        sp->pos[0]= xg*coso+yg*sino*COS_5+zg*sino*SIN_5;
        sp->pos[1]=-xg*sino+yg*coso*COS_5+zg*coso*SIN_5;
        sp->pos[2]=-yg*SIN_5+zg*COS_5;
    }
    else {
        O=eph->OMG0+(eph->OMGd-omge)*tk-omge*eph->toes;
        sinO=sin(O); cosO=cos(O);
        //std::cout << "x: " << x*cosO-y*cosi*sinO << "y: " << x*sinO+y*cosi*cosO << "z: " << y*sin(i) << std::endl;
        sp->pos[0]=x*cosO-y*cosi*sinO;
        sp->pos[1]=x*sinO+y*cosi*cosO;
        sp->pos[2]=y*sin(i);
    }
    tk=timediff(time,eph->toc);
    dts=eph->f0+eph->f1*tk+eph->f2*tk*tk;

    /* relativity correction */
    dts-=2.0*sqrt(mu*eph->A)*eph->e*sinE/SQR(CLIGHT);

    /* position and clock error variance */
    var=var_uraeph(sys,eph->sva);
}

double A;
double M_0;
double sqrtA;
double e;
double dn;

double EccentricAnomaly(double t_k)
{
  // Semi-major axis
  A = sqrtA*sqrtA;

  // Computed mean motion (rad/sec)
  double n_0 = sqrt(MU/(A*A*A));

  // Corrected mean motion
  double n = n_0 + dn;

  // Mean anomaly
  double M_k = M_0 + n*t_k;

  // Solve Kepler's Equation for Eccentric Anomaly
  double E_k = M_k;
  for(;;)
  {
      double temp = E_k;
      E_k = M_k + e*sin(E_k);
      if (fabs(E_k - temp) < 1e-10) break;
  }

  return E_k;
}
