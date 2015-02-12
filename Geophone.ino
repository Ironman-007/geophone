/****************************************************************************
 * geophone.ino
 *
 * Record geophone data and create a time vs. frequency profile.
 * The geophone data is sampled from the amplifier board and analyzed for'
 * frequency content by removing any frequency components whose magnitude
 * is below a threshold value.
 *
 * The analysis is provided as a time stamp (in plain text) followed by
 * binary frequency component pairs.  Each frequency component pair consists
 * of the frequency bin (0 through 255) and its corresponding amplitude
 * (-32767 through 32767).
 *
 * The software requires an Arduino with at least 4096 bytes of RAM such as
 * the Arduino Mega or the Arduino Due.
 *
 * See the file COPYRIGHT for copyright details.  In addition, the bit-
 * reversal function is based on "Katja's homepage on sinusoids,
 * complex numbers and modulation" at <http://www.katjaas.nl/home/home.html>.
 ***************************************************************************/

#include <math.h>
#if defined( ARDUINO_AVR_MEGA2560 )
#include <EEPROM.h>
#endif


/* Serial speed for the report generation.  It should be fast enough to
   allow several values to be passed per second.  A speed of 38,400 baud
   should suffice for worst case reports of about 2,300 bytes. */
#define SERIAL_SPEED    115200

/* The geophone data is sampled on analog pin 5. */
#define GEODATA_PIN          5

/* Report only frequency components within the range of the geophone sensor
   and the amplifier's bandpass setting.  For example with an SM-24 geophone
   measuring the range between 10 Hz to 240 Hz and an amplifier with a band-
   pass filter from 7 Hz to 150 Hz, the range is 10 Hz to 150 Hz.
*/
#define LOWEST_FREQUENCY_REPORTED    10
#define HIGHEST_FREQUENCY_REPORTED  150

/* Make an LED blink on every successful report. */
#define REPORT_BLINK_ENABLED   1
#define REPORT_BLINK_LED_PIN  13

/* Default threshold for reporting amplitudes. */
#define DEFAULT_AMPLITUDE_THRESHOLD  0.5

/* Define the geophone data sampling rate. */
#define SAMPLE_RATE   512

/* Include bit-reversed twiddle factors for a 512-point radix-2 DIT FFT. */
#define NUMBER_OF_GEODATA_SAMPLES 512
#include "twiddle_factors_256_br.h"

/* Define the on-board LED so we can turn it off. */
#define LED_PIN             13

/* EEPROM address where the amplitude threshold is stored.  Sadly, the Arduino
   Due doesn't have EEPROM. */
#define AMPLITUDE_THRESHOLD_EEPROM_ADDRESS 0


/* Create a double buffer for geodata samples so that the frequency analysis
 * may be performed on one buffer while the other is being filled with samples.
 * The imaginary part requires only one buffer.
 */
short geodata_samples[ NUMBER_OF_GEODATA_SAMPLES * 2 ];
short *geodata_samples_real;
short geodata_samples_imag[ NUMBER_OF_GEODATA_SAMPLES ];
/* Indexes used by the interrupt service routine. */
int  isr_hamming_window_index;
int  isr_current_geodata_index;
/* Semaphor indicating that a frame of geophone samples is ready. */
bool geodata_buffer_full;
/* Current threshold at which amplitudes are reported. */
double amplitude_threshold;
/* Flag that indicates that a report with amplitude information was
   created.  It is used by the report LED blinking. */
bool report_was_created;


/**
 * Setup the timer interrupt and prepare the geodata sample buffers for
 * periodic sampling.  Timer1 is used to generate interrupts at a rate of
 * 512 Hz.
 *
 * This function is board specific; if other board than the Arduino Mega
 * or the Arduino Due are used the code must be updated.
 */
void start_sampling( )
{
  /* Prepare the buffer for sampling. */
  isr_hamming_window_index  = 0;
  isr_current_geodata_index = 0;
  geodata_buffer_full       = false;

  /* Setup interrupts for the Arduino Mega. */
#if defined( ARDUINO_AVR_MEGA2560 )
  // Set timer1 interrupt to sample at 512 Hz. */
  const unsigned short prescaling     = 1;
  const unsigned short match_register = 16000000ul / ( prescaling * SAMPLE_RATE ) - 1;

  cli( );
  TCCR1A = 0;
  TCCR1B = 0;
  TCNT1  = 0;
  OCR1A = match_register;
  /* turn on CTC mode. */
  TCCR1B |= (1 << WGM12);
  /* Clear CS10, CS11, and CS12 bits for prescaling = 1. */
  TCCR1B &= 0xffff - ( 1 << CS12 ) | ( 1 << CS11 ) | ( 1 << CS10 );
  /* Enable timer compare interrupt. */
  TIMSK1 |= ( 1 << OCIE1A );
  sei( );

  /* Setup interrupts the Arduino Due. */
#elif defined( ARDUINO_SAM_DUE )
  /* Set a 12-bit resolutiong. */
  analogReadResolution( 12 );
  /* Disable write protect of PMC registers. */
  pmc_set_writeprotect( false );
  /* Enable the peripheral clock. */
  pmc_enable_periph_clk( TC3_IRQn );
  /* Configure the channel. */
  TC_Configure( TC1, 0, TC_CMR_WAVE | TC_CMR_WAVSEL_UP_RC | TC_CMR_TCCLKS_TIMER_CLOCK4 );
  uint32_t rc = VARIANT_MCK/128/SAMPLE_RATE;
  /* Setup the timer. */
  TC_SetRA( TC1, 0, rc/2 );
  TC_SetRC( TC1, 0, rc );
  TC_Start( TC1, 0 );
  TC1->TC_CHANNEL[ 0 ].TC_IER = TC_IER_CPCS;
  TC1->TC_CHANNEL[ 0 ].TC_IDR = ~TC_IER_CPCS;
  NVIC_EnableIRQ( TC3_IRQn );
#else
#error Arduino board not supported by this software.
#endif
}



#if defined( ARDUINO_AVR_MEGA2560 )
/**
 * Interrupt service routine for Arduino Mega devices which invokes the
 * generic interrupt service routine.
 */
ISR( TIMER1_COMPA_vect )
{
  sampling_interrupt( );
}


#elif defined( ARDUINO_SAM_DUE )
/**
 * Interrupt service routine for Arduino Due devices which invokes the
 * generic interrupt service routine.
 */
void TC3_Handler( )
{
  TC_GetStatus( TC1, 0 );
  sampling_interrupt( );
}


#else
#error Arduino board not supported by this software.
#endif



/*
 * Interrupt service routine for sampling the geodata.  The geodata analog
 * pin is sampled at each invokation of the ISR.  If the buffer is full, a
 * pointer is passed to the main program and a semaphor is raised to indicate
 * that a new frame of samples is available, and future samples are written
 * to the other buffer.
 *
 * While not a sampling task, we take advantage of the timer interrupt to
 * blink the report LED if enabled.
 */
void sampling_interrupt( )
{
  /* Read a sample and store it in the geodata buffer.  Apply a Hamming
     window as we go along.  It involves a cos operation; the alternative
     is an array that should be fit into program memory. */
#if defined( ARDUINO_AVR_MEGA2560 )
  const int adc_resolution = 1024;
#elif defined( ARDUINO_SAM_DUE )
  const int adc_resolution = 4096;
#endif
  short geodata_sample = analogRead( GEODATA_PIN ) - ( adc_resolution >> 1 );
  const double alpha = 0.54;
  const double beta  = 1.0 - alpha;
  const double N     = (double)( NUMBER_OF_GEODATA_SAMPLES - 1 );
  float hamming_window =
    alpha - beta * cos( 2.0 * M_PI * (double)isr_hamming_window_index / N );
  isr_hamming_window_index++;
  /* Scale the sample. */
  const double scale = 8192.0 / adc_resolution;
  geodata_sample = (short)( (double)geodata_sample * hamming_window * scale );
  geodata_samples[ isr_current_geodata_index++ ] = geodata_sample;

  /* Raise a semaphor if the buffer is full and tell which buffer
     is active. */
  if( isr_current_geodata_index == NUMBER_OF_GEODATA_SAMPLES )
  {
    geodata_samples_real     = &geodata_samples[ 0 ];
    isr_hamming_window_index = 0;
    geodata_buffer_full      = true;
  }
  else if( isr_current_geodata_index == NUMBER_OF_GEODATA_SAMPLES * 2 )
  {
    geodata_samples_real      = &geodata_samples[ NUMBER_OF_GEODATA_SAMPLES ];
    isr_current_geodata_index = 0;
    isr_hamming_window_index  = 0;
    geodata_buffer_full       = true;
  }

  /* In the same interrupt routine, handle report LED blinking. */
  report_blink( REPORT_BLINK_ENABLED );
}



/**
 * Blink the report LED if it has been enabled.
 *
 * @param enabled @a true if report blinking has been enabled.
 */
void report_blink( bool enabled )
{
  static unsigned long timestamp;
  static bool          led_on = false;

  if( enabled == true )
  {
    /* Turn on the LED and start a timer if a report was created. */
    if( report_was_created == true )
    {
      report_was_created = false;
      timestamp = millis( ) + 125;
      digitalWrite( REPORT_BLINK_LED_PIN, HIGH );
      led_on = true;
    }
    /* Turn off the LED once the timer expires. */
    if( led_on == true )
    {
      if( millis( ) > timestamp )
      {
        digitalWrite( REPORT_BLINK_LED_PIN, LOW );
        led_on = false;
      }
    }
  }
}



/**
 * Compute the amplitude based on a real and an imaginary component.
 *
 * @param [in] real Real component.
 * @param [in] real Imaginary component.
 * @return Amplitude.
 */
double compute_amplitude( double real, double imaginary )
{
  double amplitude = sqrt( real * real + imaginary * imaginary );
  return( amplitude );
}



/**
 * Swap two complex numbers in an array.
 *
 * @param [in] first Index of the first number.
 * @param [in] second Index of the other number.
 * @param [in,out] real Array of real components.
 * @param [in,out] imaginary Array of imaginary components.
 */
void swap( int first, int second, short *real, short *imaginary )
{
  short temp_r = real[ first ];                
  real[ first ] = real[ second ];
  real[ second ] = temp_r;

  short temp_i = imaginary[ first ];                
  imaginary[ first ] = imaginary[ second ];
  imaginary[ second ] = temp_i;
}



/**
 * Bit-reverse the elements in a complex array.
 *
 * @param [in,out] real Array of real components.
 * @param [in,out] imag Array of imaginary components.
 * @param [in] length Length of the array.
 */
void bit_reverse_complex( short *real, short *imag, int length )
{    
  int N = length;

  int halfn  = N >> 1;
  int quartn = N >> 2;
  int nmin1  = N - 1;

  unsigned int forward = halfn;
  unsigned int rev     = 1;

  /* Start of bit-reversed permutation loop, N/4 iterations. */
  for( int i = quartn; i; i-- )
  {
    /* Gray code generator for even values. */
    unsigned int nodd = ~i;                  // counting ones is easier
    int zeros;
    for( zeros = 0; nodd & 1; zeros++ )
    {
      nodd >>= 1;   // find trailing zeros in i
    }
    forward ^= 2 << zeros;      // toggle one bit of forward
    rev     ^= quartn >> zeros; // toggle one bit of rev

    /* Swap even and ~even conditionally. */
    if( forward < rev )
    {
      swap( forward, rev, real, imag );
      nodd = nmin1 ^ forward; // compute the bitwise negations
      unsigned int noddrev = nmin1 ^ rev;        
      swap( nodd, noddrev, real, imag ); // swap bitwise-negated pairs
    }

    nodd = forward ^ 1;    // compute the odd values from the even
    unsigned int noddrev = rev ^ halfn;
    swap( nodd, noddrev, real, imag );  // swap odd unconditionally
  }    
}



/**
 * Q.15  fractional integer 16-bit x 16-bit -> 16-bit multiplication.
 *
 * @param [in] a The first q.15 fractional integer factor.
 * @param [in] b The other q.15 fractional integer factor.
 * @return Product in q.15 fractional integer format.
 */
inline short q15_mul16( short a, short b )
{
  long product = a * b;
  return( (short)( ( product >> 15 ) & 0x0000ffff ) );
}



/**
 * Complex addition using q.15 fractional arithmetic.
 *
 * @param [out] res_r Real part of complex sum.
 * @param [out] res_i Imaginary part of complex sum.
 * @param [in] r1 Real part of first complex number.
 * @param [in] i1 Imaginary part of first complex number.
 * @param [in] r2 Real part of second complex number.
 * @param [in] i2 Imaginary part of second complex number.
 */
inline void complex_add( short *res_r, short *res_i,
                         short r1, short i1, short r2, short i2 )
{
  *res_r = r1 + r2;
  *res_i = i1 + i2;
}



/**
 * Complex multiplication using q.15 fractional arithmetic.
 *
 * @param [out] res_r Real part of complex product.
 * @param [out] res_i Imaginary part of complex product.
 * @param [in] r1 Real part of first complex factor.
 * @param [in] i1 Imaginary part of first complex factor.
 * @param [in] r2 Real part of second complex factor.
 * @param [in] i2 Imaginary part of second complex factor.
 */
inline void complex_mul( short *res_r, short *res_i,
                         short r1, short i1, short r2, short i2 )
{
  *res_r = q15_mul16( r1, r2 ) - q15_mul16( i1, i2 );
  *res_i = q15_mul16( r1, i2 ) + q15_mul16( i1, r2 );
}



/**
 * Perform a 512-point radix-2 in-place DIT FFT using q.15 fractional
 * arithmetic.
 *
 * @param [in,out] data_real Array of real components of complex input/output.
 * @param [in,out] data_imag Array of imaginary components of complex
 *        input/output.
 */ 
void fft_radix2_512( short *data_real, short *data_imag )
{
  const int length = 512;

  int pairs_per_group  = length / 2;
  int wingspan         = length / 2;

  /* Divide and conquer. */
  for( int stage = 1; stage < length; )
  {
    for( int group = 0; group < stage; group++ )
    {
      /* Read the twiddle factors for the curent group. */
      short WR = pgm_read_word_near( twiddle_real + group );
      short WI = pgm_read_word_near( twiddle_imag + group );

      /* Calculate the positions of the butterflies in this group. */
      int lower = 2 * group * pairs_per_group;
      int upper = lower + pairs_per_group;

      /* Compute all the butterflies in the current group. */
      for( int butterfly = lower; butterfly < upper; butterfly++ )
      {
        /* Compute one FFT butterfly. */
        short temp_r, temp_i;
        complex_mul( &temp_r, &temp_i, WR, WI,
                     data_real[ butterfly + wingspan ],
                     data_imag[ butterfly + wingspan ]);
        complex_add( &data_real[ butterfly + wingspan ],
                     &data_imag[ butterfly + wingspan ],
                     data_real[ butterfly ], data_imag[ butterfly ],
                     -temp_r, -temp_i );
        complex_add( &data_real[ butterfly ], &data_imag[ butterfly ],
                     data_real[ butterfly ], data_imag[ butterfly ],
                     temp_r, temp_i );
      }
    }

    pairs_per_group = pairs_per_group >> 1;
    stage           = stage << 1;
    wingspan        = wingspan >> 1;
  }
}



/**
 * Print an array of amplitudes that are above the specified threshold.
 *
 * @param [in] freq_real Array of real components of the frequency bins.
 * @param [in] freq_imag Array of imaginary components of the frequency bins.
 * @param [in] length Number of frequency bins.
 * @param [in] threshold Threshold for reporting an amplitude component.
 */
void report( const short *freq_real, const short *freq_imag, int length,
             double threshold )
{
  bool first_entry = true;

  /* Walk through the frequency components array and repotr any frequency
     whose amplitude is above the specified threshold. */
  const int lower_frequency_bin =
    LOWEST_FREQUENCY_REPORTED * NUMBER_OF_GEODATA_SAMPLES / SAMPLE_RATE;
  const int upper_frequency_bin =
    HIGHEST_FREQUENCY_REPORTED * NUMBER_OF_GEODATA_SAMPLES / SAMPLE_RATE;
  for( int frequency_bin = lower_frequency_bin;
       frequency_bin <= upper_frequency_bin;
       frequency_bin++ )
  {
    /* Compute the amplitude. */
    double real = (double)freq_real[ frequency_bin ] / 32768.0;
    double imag = (double)freq_imag[ frequency_bin ] / 32768.0;
    double amplitude = sqrt( real * real + imag * imag );
    /* Report the frequency bin and the amplitude if the threshold is
       exceeded. */
    if( amplitude >= threshold )
    {
      /* Comma-separate the numbers. */
      if( first_entry == true )
      {
        first_entry = false;
      }
      else
      {
        Serial.print( "," );
      }
      /* Print the frequency bin and its amplitude. */
      double frequency =
        (double)SAMPLE_RATE / (double)NUMBER_OF_GEODATA_SAMPLES
        * (double)frequency_bin + 0.5;
      Serial.print( (short)frequency );
      Serial.print( "," );
//      Serial.print( (short)( amplitude * 32768.0) );
      Serial.print( amplitude, 4 );
    }
  }
  /* Terminate the report if any output was reported and indicate to the
     report LED blinking that the report was submitted. */
  if( first_entry == false )
  {
    Serial.println( "" );
	report_was_created = true;
  }
}



/**
 * Read the current threshold value from EEPROM.  Use the default value if no
 * value has been stored in EEPROM.
 *
 * @return Stored (or default) threshold value.
 */
double read_amplitude_threshold_from_eeprom( )
{
  double        threshold;
  unsigned char *threshold_bytes = (unsigned char*)&threshold;
  for( int i = 0; i < sizeof( double ); i++ )
  {

#if defined( ARDUINO_AVR_MEGA2560 )
    byte value = EEPROM.read( AMPLITUDE_THRESHOLD_EEPROM_ADDRESS + i );
#else
    byte value = 0;
#endif
    threshold_bytes[ i ] = value;
  }
  if( threshold == 0.0 )
  {
    threshold = DEFAULT_AMPLITUDE_THRESHOLD;
  }
  return( threshold );
}



/**
 * Save a new threshold value to EEPROM.
 *
 * @param New threshold value to store in EEPROM.
 */
void save_amplitude_threshold_to_eeprom( double threshold )
{
#if defined( ARDUINO_AVR_MEGA2560 )
  unsigned char *threshold_bytes = (unsigned char*)&threshold;
  for( int i = 0; i < sizeof( double ); i++ )
  {
    byte value = threshold_bytes[ i ];
    EEPROM.write( AMPLITUDE_THRESHOLD_EEPROM_ADDRESS + i, value );
  }
#endif
}



/**
 * Flush the serial input buffer.
 */
void flush_serial_input( )
{
  while( Serial.available( ) > 0 )
  {
    Serial.read( );
  }
}



/**
 * Read a new threshold value from the serial port.  The function is invoked
 * regularly and reads a byte from the serial port, if any.  If a proper
 * value is read (within a short time to avoid spurious bytes eventually
 * forming a value), return it.  Otherwise return a negative value, which is
 * an invalid amplitude.
 *
 * The function responds with "OK" if the number was successfully read or
 * provides a brief error message otherwise.
 *
 * @return New threshold value, or negative if no new value was provided.
 */
double get_new_threshold( )
{
  static char          threshold_string[ 20 ];
  static int           threshold_string_pos = 0;
  static unsigned long timestamp;
  /* Timeout is 250 ms. */
  const unsigned long  timeout = 250;

  /* Timeout if characters are not being received swiftly enough. */
  if( ( threshold_string_pos > 0 ) && ( timestamp + timeout < millis( ) ) )
  {
    Serial.println( "E:TIMEOUT" );
    flush_serial_input( );
    threshold_string_pos = 0;
  }

  /* Read the next byte from the serial port, if any. */
  else if( Serial.available( ) > 0 )
  {
    /* The first byte should start a timeout counter.  The entire string
       is discarded if the timer expires before the value has been
       submitted to the device. */
    if( threshold_string_pos == 0 )
    {
      timestamp = millis( );
    }

    if( threshold_string_pos < sizeof( threshold_string ) - 1 )
    {
      char incoming_byte = Serial.read( );
      /* Attempt to parse the new threshold value once a newline is received. */
      if( incoming_byte == '\r' || incoming_byte == '\n' )
      {
        threshold_string[ threshold_string_pos ] = '\0';
        double threshold = atof( threshold_string );
        /* Only threshold values between 0.0 and 1.0 are allowed. */
        if( threshold < 0.0 || threshold >= 1.0 )
        {
          threshold = -1.0;
          Serial.println( "E:OVERFLOW" );
        }
        /* The threshold value is valid so report success. */
        else
        {
          Serial.print( "OK:" );
          Serial.println( threshold );
        }
        flush_serial_input( );
        threshold_string_pos = 0;
        return( threshold );
      }
      /* Add the byte to the threshold string if it's a valid numerical
         character. */
      else if(    ( ( incoming_byte >= '0' ) && ( incoming_byte <= '9' ) )
               || ( incoming_byte == '.' ) )
      {
        threshold_string[ threshold_string_pos++ ] = incoming_byte;
      }
      /* Any invalid byte resets the string. */
      else
      {
        Serial.println( "E:NON-NUMERIC" );
        flush_serial_input( );
        threshold_string_pos = 0;
      }
    }
    /* Too much junk in the input buffer; discard everything. */
    else
    {
      Serial.println( "E:FLUSH" );
      flush_serial_input( );
      threshold_string_pos = 0;
    }
  }

  return( -1.0 );
}



/**
 * Initialize the amplitude threshold from last run and initialize the
 * serial port.  Also, turn off the on-board LED.
 */
void setup()
{
  /* Read the amplitude threshold value from EEPROM or use the default value
     if nothing was stored in EEPROM. */
  amplitude_threshold = read_amplitude_threshold_from_eeprom( );

  /* Initialize the serial port with the desired speed. */
  Serial.begin( SERIAL_SPEED );

  /* Setup the geophone data sampling buffers and sampling interrupt. */
  start_sampling( );

  /* Turn off the on-board LED. */
  pinMode( LED_PIN, OUTPUT );
  digitalWrite( LED_PIN, LOW );

  /* Configure the report LED if enabled. */
  report_was_created = false;
  if( REPORT_BLINK_ENABLED )
  {
    pinMode( REPORT_BLINK_LED_PIN, OUTPUT );
    digitalWrite( REPORT_BLINK_LED_PIN, LOW );
  }
}



/**
 * Main program loop which performs a frequency analysis each time the
 * geophone sample buffer has been filled and creates a report with the
 * frequency/amplitude data.  The main loop also listens for new amplitude
 * threshold values submitted via the serial port.
 */
void loop()
{
  /* Analyze the geophone data once it's available. */
  if( geodata_buffer_full == true )
  {
    geodata_buffer_full = false;

    /* Compute the Fourier transform in-place. */
	bzero( geodata_samples_imag, 2 * NUMBER_OF_GEODATA_SAMPLES );
    fft_radix2_512( geodata_samples_real, &geodata_samples_imag[ 0 ] );
    bit_reverse_complex( geodata_samples_real, &geodata_samples_imag[ 0 ],
                         NUMBER_OF_GEODATA_SAMPLES );

    /* Compute the amplitudes and report them in the same run.  Since
       the input data is real, the last half of the Fourier transform is
       is the complex conjugate of the first half and thus redundant.
       (This is why we bother to bit-reverse the Fourier-transformed
       data:  otherwise the redundant information would be spread out
       in the output data and would require bit-reversed addressing to
       detect and eliminate anyway.) */
    report( geodata_samples_real, &geodata_samples_imag[ 0 ],
            NUMBER_OF_GEODATA_SAMPLES / 2 + 1, amplitude_threshold );
  }

  /* Read any new threshold value that may be provided.  Update the threshold
     and write it to EEPROM if the value is valid. */
  double threshold = get_new_threshold( );
  if( threshold > 0.0 )
  {
    save_amplitude_threshold_to_eeprom( threshold );
    amplitude_threshold = threshold;
  }
}
