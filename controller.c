// ---------- Máquina de estados -----------

#include "IO.c"


#undef DEBUG
#define DEBUG

// Tipos

// Estados da máquina
typedef enum {
	STOP1,
	MOV1,
	WAIT1_BOX,
	WAIT1,
	PRESS_DOWN,
	PRESS_WAIT,
	PRESS_UP,
	EMPURRA,
	RECOLHE,
} stateNames1;

typedef enum {
	STOP2,
	MOV2_IN,
	MOV2_OUT,
	CONTADOR,
} stateNames2;


// Funções
void initME();

// Estado atual da máquina
stateNames1 currentState1;
stateNames2 currentState2;
u_int64_t tempo = 0;
int count = 0;
int st_prev = 0;


// Inicializa a ME
void init_ME()
{
	
	// Estado inicial
	currentState1 = STOP1;
	currentState2 = STOP2;

	
	// Saídas
	T1 = 0;
	LP = 0;
	LV = 0;
	E1 = 0;
	E2 = 0;
	T2 = 0;

	count = 0;
	st_prev = 0;

}

// Código principal
int main() {


	// Inicialização da ME
	init_ME();
	
	// Ciclo de execução
	while(1) {

		#ifdef DEBUG
		//printf ("\n*** Inicio do Ciclo ***\n");
		#endif

		// Leitura das entradas
		read_inputs();

		int st_fall = (ST == 0 && st_prev == 1);
		st_prev = ST;

		// Transição entre estados
		switch (currentState1) 
		{
		
			
			case STOP1:
					
				// Testa transição STOP -> MOV
				if (BA == 1 && SS == 0) {
					
					// Próximo estado
					currentState1 = MOV1;
					
					#ifdef DEBUG
					printf ("\n*** Tapete em movimento ***\n");
					#endif
				}
			
				break;

			case MOV1 :
				
				if (SC == 1 && SS == 1) {

					currentState1 = WAIT1_BOX;
					break;

				}

				if (SS == 1) {
					
					currentState1 = WAIT1;
					#ifdef DEBUG
					printf ("\n*** Tapete parado ***\n");
					#endif
				}
			
					
			break;

			case WAIT1:

				if  (SS == 0) 
				{

					if (BSA == 0 && BSV == 0) {
            			break;
        			}

					if ( ( SV == 4 && BSV == 1 && BSA == 0) || (SV == 1 && BSA == 1 && BSV == 0) ) {
						currentState1 = PRESS_DOWN;
					} else if (( SV == 4 && BSV == 0) || (SV == 1 && BSA == 0) ){
						currentState1 = EMPURRA;
					}
				
				}

			break;


			case PRESS_DOWN:
				
				if ( SZ == 0 ) {

					tempo = get_time();
					currentState1 = PRESS_WAIT;

				}
			
			break;

			case PRESS_WAIT:
				
				if ( get_time() - tempo  >= 1500 ) {

					currentState1 = PRESS_UP;

				}
			
			break;

			case PRESS_UP:

				if ( SZ == 0 ) {

					if (SC == 1) {

						currentState1 = WAIT1_BOX;
					} else {

						currentState1 = MOV1;
						
					}

				}

			break;

			case EMPURRA:

				if (SIE == 1) {

					currentState1 = RECOLHE;

				}
			
			break;

			case RECOLHE:
				
				if (SIR == 1) {

					currentState1 = MOV1;

				}

			break;

			case WAIT1_BOX:

				if (SC == 0) {

					currentState1 = MOV1;

				}
			
			break;
				
		} //end case

		switch (currentState2)
		{
			case STOP2:


				if (BA == 1 && SC == 1) {

					currentState2 = MOV2_IN;

				}

			break;

			case MOV2_IN: 

				if (SC == 0) {

					count = 0;
					currentState2 = CONTADOR;

				}

			break;

			case CONTADOR:

				if(st_fall) {

					count++;

				}

				if( count >= 3 && ST == 0) {

					CC++;
					currentState2 = MOV2_OUT;

				}
			
			break;

			case MOV2_OUT:

				if( SC == 1 ) {

					currentState2 = MOV2_IN;

				}

			break;

		}
		

		// Atualiza saídas

		T1 = (currentState1 == MOV1)  || (currentState1 == WAIT1 && SS == 1);
		E1 = T1;
		LP = (currentState1 == STOP1);
		LV = (BSV == 1);
		LA = (BSA == 1);
		MZ = (currentState1 == PRESS_DOWN || currentState1 == PRESS_WAIT);
		IP = (currentState1 == EMPURRA);

		T2 = (currentState2 == MOV2_IN) || (currentState2 == MOV2_OUT);
		E2 = T2;
		
		//Escrita nas saídas
		write_outputs();
		
	} // end loop
	
}// end main