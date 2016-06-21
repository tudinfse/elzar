/*****************************************************************************
 * fft: Fast Fourier Transform.
 *****************************************************************************
 * Source: http://web.mit.edu/~emin/Desktop/ref_to_emin/www.old/source_code/fft/index.html
 * Author: Emin Martinian
 *
 * INPUT: float input[16], float output[16]
 * OUTPUT: none
 * EFFECTS:  Places the 16 point fft of input in output in a strange
 * order using 10 real multiplies and 79 real adds.
 * Re{F[0]}= out0
 * Im{F[0]}= 0
 * Re{F[1]}= out8
 * Im{F[1]}= out12
 * Re{F[2]}= out4
 * Im{F[2]}= -out6
 * Re{F[3]}= out11
 * Im{F[3]}= -out15
 * Re{F[4]}= out2
 * Im{F[4]}= -out3
 * Re{F[5]}= out10
 * Im{F[5]}= out14
 * Re{F[6]}= out5
 * Im{F[6]}= -out7
 * Re{F[7]}= out9
 * Im{F[7]}= -out13
 * Re{F[8]}= out1
 * Im{F[8]}=0
 *
 * F[9] through F[15] can be found by using the formula
 * Re{F[n]}=Re{F[(16-n)mod16]} and Im{F[n]}= -Im{F[(16-n)mod16]}
 * Note using temporary variables to store intermediate computations
 * in the butterflies might speed things up.  When the current version
 * needs to compute a=a+b, and b=a-b, I do a=a+b followed by b=a-b-b.
 *
 * So practically everything is done in place, but the number of adds
 * can be reduced by doinc c=a+b followed by b=a-b.
 * The algorithm behind this program is to find F[2k] and F[4k+1]
 * seperately.  To find F[2k] we take the 8 point Real FFT of x[n]+x[n+8]
 * for n from 0 to 7.  To find F[4k+1] we take the 4 point Complex FFT of
 * exp(-2*pi*j*n/16)*{x[n] - x[n+8] + j(x[n+12]-x[n+4])} for n from 0 to 3.
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>

extern double output[16];

void R16SRFFT(double *input);

int main(int argc, char *argv[])
{
    double data[16];
    double zero=0;

    // get intput
    if(argc != 17) {
        printf("16 point vector should be supplied!\n");
        return 1;
    } else {
        int i;
        for (i = 0; i < 16; i ++) {
            data[i] = atof(argv[i + 1]);
        }
    }

    long i = 0;
    for (; i < 50000000; i++) {
        // for perf tests
        R16SRFFT(data);
    }

    // print results
    printf("\nresult is:\n");
    printf("k,\t\tReal Part\t\tImaginary Part\n");
    printf(" 0\t\t%.9f\t\t%.9f\n", output[0], zero);
    printf(" 1\t\t%.9f\t\t%.9f\n", output[1], output[9]);
    printf(" 2\t\t%.9f\t\t%.9f\n", output[2], output[10]);
    printf(" 3\t\t%.9f\t\t%.9f\n", output[3], output[11]);
    printf(" 4\t\t%.9f\t\t%.9f\n", output[4], output[12]);
    printf(" 5\t\t%.9f\t\t%.9f\n", output[5], output[13]);
    printf(" 6\t\t%.9f\t\t%.9f\n", output[6], output[14]);
    printf(" 7\t\t%.9f\t\t%.9f\n", output[7], output[15]);
    printf(" 8\t\t%.9f\t\t%.9f\n", output[8], zero);
    printf(" 9\t\t%.9f\t\t%.9f\n", output[7], -output[15]);
    printf("10\t\t%.9f\t\t%.9f\n", output[6], -output[14]);
    printf("11\t\t%.9f\t\t%.9f\n", output[5], -output[13]);
    printf("12\t\t%.9f\t\t%.9f\n", output[4], -output[12]);
    printf("13\t\t%.9f\t\t%.9f\n", output[3], -output[11]);
    printf("14\t\t%.9f\t\t%.9f\n", output[2], -output[9]);
    printf("15\t\t%.9f\t\t%.9f\n", output[1], -output[8]);
    return 0;
}
