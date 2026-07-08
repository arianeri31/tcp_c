#!/bin/bash

# Ce script sert à remettre le CPU dans un état plus normal après les benchmarks.
#
# Le setup force le CPU dans un mode plus stable/performance.
# C'est utile pour mesurer, mais ce n'est pas forcément ce qu'on veut garder
# tout le temps, parce que ça peut consommer plus et chauffer davantage.
#
# Ici, on remet donc :
# - le turbo autorisé
# - une plage de performance normale
# - un governor plus économe
# - une préférence énergie plus équilibrée

# On vérifie que le script est lancé avec sudo.
# C'est nécessaire parce qu'on écrit dans /sys.
if [ "$EUID" -ne 0 ]; then
    echo "Il faut lancer ce script avec sudo :"
    echo "sudo ./teardown_cpu.sh"
    exit 1
fi

echo "=== Teardown CPU après les benchmarks ==="
echo

# ------------------------------------------------------------
# Partie 1 : intel_pstate
# ------------------------------------------------------------
#
# On restaure les réglages intel_pstate.
#
# no_turbo = 0 réautorise le turbo.
# min_perf_pct = 0 et max_perf_pct = 100 redonnent au CPU une plage normale.
# ------------------------------------------------------------

if [ -d /sys/devices/system/cpu/intel_pstate ]; then
    echo "intel_pstate détecté"

    # On réactive le turbo.
    #
    # no_turbo = 0 => turbo autorisé
if [ -e /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
    if echo 0 > /sys/devices/system/cpu/intel_pstate/no_turbo; then
        echo "Turbo réactivé"
    else
        echo "Impossible de réactiver le turbo"
        echo "L'écriture dans le no_turbo est refusée"
    fi
fi

    # On remet une plage de performance normale.
    #
    # max_perf_pct = 100 => le CPU peut monter jusqu'à 100 %
    # min_perf_pct = 0   => le CPU peut redescendre si besoin
    #
    # Cela permet de retrouver un comportement plus classique après les tests.
    if [ -e /sys/devices/system/cpu/intel_pstate/max_perf_pct ]; then
        echo 100 > /sys/devices/system/cpu/intel_pstate/max_perf_pct
        echo 0 > /sys/devices/system/cpu/intel_pstate/min_perf_pct

        echo "Plage de performance intel_pstate restaurée"
    fi

    echo
    echo "Valeurs actuelles de intel_pstate :"
    grep "" /sys/devices/system/cpu/intel_pstate/* 2>/dev/null || true
    echo
fi

# ------------------------------------------------------------
# Partie 2 : cpufreq
# ------------------------------------------------------------
#
# On restaure les réglages cpufreq.
#
# Pendant le setup, on a essayé de fixer la fréquence pour les benchmarks.
# Ici, on redonne au CPU la possibilité de varier sa fréquence normalement.
# ------------------------------------------------------------

if [ -d /sys/devices/system/cpu/cpufreq ]; then
    echo "cpufreq détecté"
    echo

    for p in /sys/devices/system/cpu/cpufreq/policy[0-9]*; do
        echo "Restauration de $p"

        # On remet powersave.
        #
        # powersave permet au système de réduire la fréquence quand il n'a pas
        # besoin de performances maximales.
        #
        # C'est plus adapté pour une utilisation normale après les tests.
        echo powersave > "$p/scaling_governor"
        echo "Governor remis en powersave"

        # On restaure les limites normales de fréquence.
        #
        # cpuinfo_max_freq contient la fréquence maximale connue par le CPU.
        # cpuinfo_min_freq contient la fréquence minimale connue par le CPU.
        #
        # On remet donc :
        #   scaling_max_freq = cpuinfo_max_freq
        #   scaling_min_freq = cpuinfo_min_freq
        #
        # Comme ça, le CPU peut à nouveau varier dans toute sa plage normale.
        if [ -e "$p/scaling_max_freq" ]; then
            cat "$p/cpuinfo_max_freq" > "$p/scaling_max_freq"
            cat "$p/cpuinfo_min_freq" > "$p/scaling_min_freq"

            echo "Limites min et max restaurées"
        fi

        # On remet une préférence énergie plus équilibrée.
        #
        # balance_performance signifie qu'on ne force plus le mode performance
        # maximale, mais qu'on ne descend pas non plus forcément au mode le plus
        # économique possible.
        if [ -e "$p/energy_performance_preference" ]; then
            echo balance_performance > "$p/energy_performance_preference"
            echo "Préférence énergie remise sur balance_performance"
        fi

        echo
    done
fi

echo "=== Teardown terminé ==="