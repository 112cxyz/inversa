// The inversa subcommands beyond run and probe. Each takes the arguments after its name and
// the lib/ directory beside the app.
#ifndef INVERSA_COMMANDS_H
#define INVERSA_COMMANDS_H
int inv_bash( int argc, char **argv, const char *lib );
int inv_apk( int argc, char **argv, const char *lib );
int inv_exec_self( char **argv, char **envp );   // another inversa-core, in 4K pages when this one is
#endif
