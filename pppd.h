int pppd_start(int default_pdp, char* pdp_type, char* apn, const char *modemport, const char *user, const char *password, const char *auth_type, const char *ppp_number);
int pppd_stop(int signo);
int pppd_running(void);