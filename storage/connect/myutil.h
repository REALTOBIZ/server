/***********************************************************************/
/*  Prototypes of Functions used externally.                           */
/***********************************************************************/
#ifndef __MYUTIL__H
#define  __MYUTIL__H

enum enum_field_types PLGtoMYSQL(int type, bool dbf);
const char *PLGtoMYSQLtype(int type, bool dbf, char var = NULL);
int   MYSQLtoPLG(char *typname, char *var = NULL);
int   MYSQLtoPLG(int mytype);
char *MyDateFmt(int mytype);
char *MyDateFmt(char *typname);

#endif // __MYUTIL__H
