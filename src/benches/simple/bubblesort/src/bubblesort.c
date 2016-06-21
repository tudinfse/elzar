#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAXRANDINT  100

#define LEN_S 100
#define LEN_M 1000
#define LEN_L 10000     // 10 thousand

// test arrays of small, medium and large sizes
int a_s[LEN_S];
int a_m[LEN_M];
int a_l[LEN_L];

// perf function: sort array using bubblesort
int __attribute__ ((noinline)) bubblesort(int *array, int size) {
    int swapped;
    int i;

    int end = size - 1;
    for (i = 1; i <= end; i++)
    {
        swapped = 0;    //this flag is to check if the array is already sorted
        int j;
        int* array_p = array; // added
        int end2 = size - i - 1;
        for(j = 0; j <= end2; j++)
        {
            if(*array_p > *(array_p + 1))   // if(array[j] > array[j+1])
            {
                int temp = *array_p;        // int temp = array[j];
                array[j] = *(array_p + 1);  // array[j] = array[j+1];
                array[j+1] = temp;          // array[j+1] = temp;
                swapped = 1;
            }
            array_p = array_p + 1;          // added

//            if(array[j] > array[j+1])
//            {
//                int temp = array[j];
//                array[j] = array[j+1];
//                array[j+1] = temp;
//                swapped = 1;
//            }
        }
        if(!swapped){
            break; //if it is sorted then stop
        }

    }
    return 0;
 }

void precomputation() {}
void postcomputation() {}

int computation(int which, int size) {
    int r;
    r = bubblesort(&a_l[0], size);
    return r;
}

int main() {
    int i, j;

    int size = sizeof(a_l)/sizeof(int);
    for (j = 0; j < size; j++)
    {
        a_l[j] = rand() % MAXRANDINT;
    }

    precomputation();
    computation(3, size);
    postcomputation();

    return 0;
}
