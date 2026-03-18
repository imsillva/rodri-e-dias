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

typedef enum {
	AZUL,
	VERDE,
	INI,
} stateNames3;


// Funções
void initME();

// Estado atual da máquina
stateNames1 currentState1;
stateNames2 currentState2;
stateNames3 currentState3;
u_int64_t tempo = 0;
int count = 0;
int st_prev = 0;
int ss_prev = 0;


// Inicializa a ME
void init_ME()
{
	
	// Estado inicial
	currentState1 = STOP1;
	currentState2 = STOP2;
	currentState3 = INI;

	
	// Saídas
	T1 = 0;
	LP = 0;
	LV = 0;
	E1 = 0;
	E2 = 0;
	T2 = 0;

	count = 0;
	st_prev = 0;
	ss_prev = 0;

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

		int ss_fall = (SS == 0 && ss_prev == 1);
		ss_prev = SS;

		//Máquina 1 (Primeiro tapete)
		switch (currentState1) 
		{
		
			
			case STOP1:
					
				if (BA == 1 && SS == 0) {
					
					currentState1 = MOV1;

				}
			
				break;

			case MOV1 :

				if(ss_fall == 1){

					if(currentState3 == INI) {
					
						currentState1 = WAIT1;

					}

					if(((SV == 4) && (currentState3 == AZUL)) || ((SV == 1) && (currentState3 == VERDE))) {

						currentState1 = EMPURRA;

					}

					if(((SV == 4) && (currentState3 == VERDE)) || ((SV == 1) && (currentState3 == AZUL))) {

						currentState1 = PRESS_DOWN;

					}

				}
			
					
			break;

			case WAIT1:

				if  (SS == 0) 
				{

					if(((SV == 4) && (currentState3 == AZUL)) || ((SV == 1) && (currentState3 == VERDE))) {

						currentState1 = EMPURRA;

					}

					if(((SV == 4) && (currentState3 == VERDE)) || ((SV == 1) && (currentState3 == AZUL))) {

						currentState1 = PRESS_DOWN;

					}
				
				}

			break;


			case PRESS_DOWN:
				
				if (SZ == 0) {

					tempo = get_time();
					currentState1 = PRESS_WAIT;

				}
			
			break;

			case PRESS_WAIT:
				
				if (get_time() - tempo  >= 1500) {

					currentState1 = PRESS_UP;

				}
			
			break;

			case PRESS_UP:

				if (SZ == 0) {

					if (currentState2 == MOV2_IN) {

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

				if (currentState2 == CONTADOR) {

					currentState1 = MOV1;

				}
			
			break;
				
		}
		// end case

		//Máquina 2 (Segundo tapete)
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

				if(count >= 3 && ST == 0) {

					CC++;
					currentState2 = MOV2_OUT;

				}
			
			break;

			case MOV2_OUT:

				if(SC == 1) {

					currentState2 = MOV2_IN;

				}

			break;

		}
		// end case

		//Máquina 3 (controlo das luzes)
		switch (currentState3)
		{
			
			case AZUL:

				if ((BSA == 0) && (BSV == 0)) {

					currentState3 = INI;

				}

			break;

			case VERDE:

				if ((BSA == 0) && (BSV == 0)) {

					currentState3 = INI;

				}

			break;

			case INI:

				if (BSA == 1) {

					currentState3 = AZUL;

				} else if (BSV == 1) {

					currentState3 = VERDE;

				}

			break;

		}
		// end case
		

		// Atualiza saídas

		T1 = (currentState1 == MOV1);
		E1 = (currentState1 != STOP1);
		LP = (currentState1 == STOP1) && (currentState2 == STOP2);
		MZ = (currentState1 == PRESS_DOWN || currentState1 == PRESS_WAIT);
		IP = (currentState1 == EMPURRA);

		T2 = (currentState2 == MOV2_IN) || (currentState2 == MOV2_OUT);
		E2 = (currentState2 != STOP2);
		
		LV = (currentState3 == VERDE);
		LA = (currentState3 == AZUL);

		//Escrita nas saídas
		write_outputs();
		
	} // end loop
	
}// end main
