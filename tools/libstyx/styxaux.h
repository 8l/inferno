int styxinitsocket(void);
void styxendsocket(void);
void styxclosesocket(int);
int styxannounce(Styxserver*, char *);
void styxinitwait(Styxserver*);
int styxnewcall(Styxserver*);
int styxnewmsg(Styxserver*, int);
int styxaccept(Styxserver *server);
void styxnewclient(Styxserver*, int);
void styxfreeclient(Styxserver*, int);
char* styxwaitmsg(Styxserver*);
int styxrecv(Styxserver*, int, char*, int, int);
int styxsend(Styxserver*, int, char*, int, int);
void styxexit(int);
