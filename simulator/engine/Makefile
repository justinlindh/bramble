CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g -O2 -DBRAMBLE_SIM \
         -I../../test/stubs \
         -I../../components/routing/include \
         -I../../components/packet/include \
         -I../../components/dedup/include \
         -I../../components/crypto/include \
         -I../../components/reliability/include \
         -I../../components/fragment/include \
         -I../../components/channel/include \
         -I../../components/security/include \
         -I../../components/airtime/include \
         -I../../components/timesync/include

SIM_OBJS = sim_event.o sim_random.o sim_emitter.o sim_node.o sim_radio.o sim_scenario.o sim_metrics.o sim_anomaly.o cJSON.o

all: bramble-sim

bramble-sim: $(SIM_OBJS) main.o
	$(CC) $(CFLAGS) -o $@ $^ -lm

%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o bramble-sim

.PHONY: all clean
