/* hw3.c */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <ctype.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>

#define MAX_CLIENTS 10

extern int total_guesses;
extern int total_wins;
extern int total_losses;
extern char ** words;
char** all_words;
int num_words;
int num_words_selected = 0;
int running = 1;
int listener;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;


void signal_handler ( int sig )
{
    if ( sig == SIGUSR1 )
    {
        running = 0;
        printf( "MAIN: SIGUSR1 rcvd; Wordle server shutting down...\n" );
        close(listener);
    }
}


char* packet_builder ( char* packet, bool is_correct, short guesses, 
                       char* resp )
{
    *packet = is_correct ? 'Y' : 'N'; 
    *(packet+1) = (guesses >> 8) & 0xFF;   
    *(packet+2) = guesses & 0xFF;

    // Copy response word
    for ( ushort i = 3 ; i < 8 ; i++ ) 
        *(packet+i) = *(resp+i-3);

    return packet;
}


char* game( char* word, char* recv, short* guesses, char* prev, char* packet)
{
    char* resp = calloc( 6, sizeof( char ) );
    strcpy( resp, prev );

    /* Validate Guess from list*/
    char is_valid = false;
    for ( int i = 0 ; i < num_words ; i++ )
    {
        if ( strcmp(recv, *(all_words+i)) == 0 ) { is_valid = true; break; }
    }
    /* if invalid return to client */
    if ( !is_valid ) 
    { 
        strcpy( resp, "?????" );
        packet_builder( packet, false, *guesses, resp );
        return resp;
    }
    (*guesses)--; 
    pthread_mutex_lock(&lock);
    total_guesses++; // since valid input, we will use a guess
    pthread_mutex_unlock(&lock);

    /* Check for full match */
    if ( strcmp(recv, word) == 0 ) 
    {
        // Copy uppercase word to response
        for ( ushort i = 0 ; i < 6 ; i++ ) *(resp+i)= toupper( *(word+i) ); 
        packet_builder( packet, true, *guesses, resp );
        return resp; 
    } 

    /* Mark all letter position matches in caps */
    for ( ushort i = 0 ; i < 5 ; i++ )
    {
        if ( *(recv+i) == *(word+i) ) *(resp+i) = toupper( *(word+i) );  
    }

    for ( ushort i = 0 ; i < 5 ; i++ )
    {
        if ( isupper( *(resp+i) ) ) continue; // skip caps

        // Get get letter counts in: true word & current response
        ushort count_in_word = 0;
        ushort count_in_resp = 0;

        for ( ushort j = 0 ; j < 5 ; j++ ) 
        {
            if ( tolower( *(resp+j) ) == *(recv+i) ) count_in_resp++;
            if ( *(recv+i) == *(word+j) ) count_in_word++; 
        }

        // Mark lowcase if specific letter count is greater in true word
        if ( count_in_word > count_in_resp ) *(resp+i) = *(recv+i);
        else  *(resp+i) = '-';  // otherwise mark '-' 
    }

    packet_builder( packet, true, *guesses, resp );
    return resp;
}


void *handle_client(void *arg) 
{
    pthread_t tid = pthread_self();  
    printf( "THREAD %lu: waiting for guess\n", tid );

    int client_sd = *(int*)arg;

    /* Create 8-byte response packet */
    char* packet = calloc(8, sizeof(char));
    if ( packet == NULL ) 
    {
        perror("Error: calloc failed");
        close(client_sd);
        pthread_exit(NULL);
    }

    short guesses = 6;
    // Select the word
    pthread_mutex_lock(&lock);
    int rand_idx = rand() % num_words;
    char* word = *(all_words + rand_idx); 
    num_words_selected++;
    pthread_mutex_unlock(&lock);

    char* caps = calloc( 6, sizeof( char ) ); // version of word all caps
    strcpy( caps, word ); 
    for ( ushort i = 0 ; i < 5 ; i++ ) *(caps+i) = toupper( *(word+i) );
    
    pthread_mutex_lock(&lock);
    *(words+num_words_selected-1) = caps;  // copy selected word to global list
    pthread_mutex_unlock(&lock);

    char* prev_response = calloc( 6, sizeof( char ) );
    strcpy( prev_response, "-----" );

    /* Reading from client */
    char* buffer = calloc( 6, sizeof( char ) );
    int bytes_read;
    while ( (bytes_read = read( client_sd, buffer, 5 )) > 0 ) 
    {
        *(buffer+bytes_read) = '\0';
        printf( "THREAD %lu: rcvd guess: %s\n", tid, buffer );

        /* convert word to lower case*/
        for ( ushort i = 0 ; i < 5 ; i++ ) *(buffer+i) = tolower( *(buffer+i) ); 

        char* resp = game( word, buffer, &guesses, prev_response, packet );
        // strcpy( prev_response, resp );

        if ( guesses == 1 ) 
        {
            if ( *packet == 'Y' ) 
                printf( "THREAD %lu: sending reply: %s (%d guess left)\n", tid, resp, guesses );
            else 
                printf( "THREAD %lu: invalid guess; sending reply: %s (%d guess left)\n", tid, resp, guesses );
        }
        else
        {
            if ( *packet == 'Y' ) 
                printf( "THREAD %lu: sending reply: %s (%d guesses left)\n", tid, resp, guesses );
            else 
                printf( "THREAD %lu: invalid guess; sending reply: %s (%d guesses left)\n", tid, resp, guesses );
        }
    
        write(client_sd, packet, 8);

        /* Check for game win by matching responce */
        if ( strcmp( resp, caps ) == 0 )
        {
            printf( "THREAD %lu: game over; word was %s!\n", tid, resp );
            pthread_mutex_lock(&lock);
            total_wins++; 
            pthread_mutex_unlock(&lock);
            break;
        }
        /* Loss if no more guesses left */
        if ( (guesses == 0) ) 
        { 
            printf( "THREAD %lu: game over; word was %s!\n", tid, caps );
            pthread_mutex_lock(&lock);
            total_losses++; 
            pthread_mutex_unlock(&lock);;
            break; 
        } 
        else printf( "THREAD %lu: waiting for guess\n", tid );
    }

    if ( bytes_read == 0 ) 
    {
        printf( "THREAD %lu: client gave up; closing TCP connection...\n", tid );
        printf( "THREAD %lu: game over; word was %s!\n", tid, caps );
        pthread_mutex_lock(&lock);
        total_losses++; 
        pthread_mutex_unlock(&lock);;
    }
    else if (bytes_read == -1) perror( "ERROR: read failed" );

    free( prev_response );
    free( buffer );
    free( packet );
    free( arg );
    close( client_sd );
    pthread_exit( NULL );
}


int wordle_server( int argc, char ** argv )
{
    /* Validate args */
    if ( argc != 5 )
    {
        fprintf( stderr, "ERROR: Invalid argument(s)" );
        fprintf( stderr, "USAGE: hw3.out <listener-port> <seed> <dictionary-"
                  "filename> <num-words>" );
        return EXIT_FAILURE;
    }

    setvbuf( stdout, NULL, _IONBF, 0 );

    /* ignore signals */
    signal( SIGINT, SIG_IGN );
    signal( SIGTERM, SIG_IGN );
    signal( SIGUSR2, SIG_IGN );
    /* Redirect signal */
    signal( SIGUSR1, signal_handler );
             
    ushort port = atoi( *(argv+1) );
    int seed = atoi( *(argv+2) );
    char* path = *(argv+3) ;
    num_words = atoi( *(argv+4) );

    /* Allocate words array */
    words = realloc( words, num_words * sizeof( char* ) + 1 );
    all_words = calloc( num_words, num_words * sizeof( char* ) );

    /* Open words file */
    int fd = open( path, O_RDONLY );
    if ( fd == -1 )
    {
        fprintf( stderr, "ERROR: open() failed" );
        return EXIT_FAILURE;
    }
    printf( "MAIN: opened %s (%d words)\n", path, num_words );
    
    /* Read in words files */
    for ( int i = 0 ; i < num_words ; i++ )
    {
        char* word = calloc( 6, sizeof( char ) );
        int rc = read( fd, word, 6 );
        if ( rc == 0 ) { free( word ); break; }
        *(word+5) = '\0';  // overwrite the \n
        *(all_words + i) = word;
    }

    /* Seed Random Number generator */
    srand( seed );
    printf( "MAIN: seeded pseudo-random number generator with %d\n", seed );

    /* ================== TCP server network setup ================== */
    
    /* Create listener TCP socket file descriptor */
    listener = socket( AF_INET, SOCK_STREAM, 0 );
    if ( listener == -1 ) 
    { 
        perror( "ERROR: socket() failed" ); 
        return EXIT_FAILURE; 
    }

    /* Populate the socket structure for bind() */
    struct sockaddr_in server;
    server.sin_family = AF_INET;                  /* IPv4 */
    server.sin_addr.s_addr = htonl( INADDR_ANY ); /* allow any IP address to connect */
    server.sin_port = htons( port );

    /* Allow socket to be reused immediatly after server shutdown */
    int opt = 1; 
    if ( setsockopt( listener, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, 
         &opt, sizeof( opt ) ) )
    {
        perror("ERROR: setsockopt");
        close(listener);
        return EXIT_FAILURE;
    }

    /* Bind port with socket*/
    if ( bind( listener, (struct sockaddr *)&server, sizeof( server ) ) == -1 )
    {
        perror( "ERROR: bind() failed" );
        return EXIT_FAILURE;
    }

    /* Identify our port number as a TCP listener port */
    if ( listen( listener, MAX_CLIENTS ) == -1 )
    {
        perror( "ERROR: listen() failed" );
        return EXIT_FAILURE;
    }
    printf( "MAIN: Wordle server listening on port {%d}\n", port );


    while ( running )
    {
        /* Accept new client connection */
        struct sockaddr_in remote_client;
        int addrlen = sizeof( remote_client );

        int newsd = accept( listener, (struct sockaddr *)&remote_client,
                            (socklen_t *)&addrlen );
        if ( newsd == -1 ) 
        { 
            if ( !running ) break;
            perror( "ERROR: accept() failed" ); 
            continue; 
        }
        printf( "MAIN: rcvd incoming connection request\n" );

        /* Allocate memory for the client socket */
        int *client_socket = calloc( 1, sizeof( int ) );
        if (client_socket == NULL) 
        {
            perror("ERROR: calloc() failed");
            close(newsd);
            continue;
        }
        *client_socket = newsd;

        /* Create a new thread for each client */
        pthread_t tid;
        int rc = pthread_create( &tid, NULL, handle_client, client_socket );
        if ( rc != 0 ) 
        {
            perror( "ERROR: pthread_create" );
            close( newsd );
            continue;
        }

        /* Detach the thread so that it cleans up after itself */
        pthread_detach( tid );
    }

    for ( int i = 0 ; i < num_words ; i++ ) free( *(all_words+i) );
    free( all_words );

    return EXIT_SUCCESS;
}