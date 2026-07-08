#!/bin/bash

# Ce script sert à préparer le CPU avant de lancer les benchmarks.
#
# Le problème pendant les mesures, c'est que le CPU peut changer sa fréquence
# tout seul selon la charge, la température, le mode économie d'énergie, etc.
#
# Du coup, deux tests identiques peuvent donner des temps différents simplement
# parce que le CPU n'était pas à la même fréquence.
#
# Ici, on essaie donc de stabiliser le CPU :
# - on désactive le turbo
# - on force un niveau de performance élevé
# - on essaie de fixer la fréquence min et max à la même valeur
# - on demande au système de privilégier les performances
#
# Ce script ne lance pas les tests.
# Il prépare juste la machine avant les tests.

# On vérifie que le script est lancé avec sudo.
# C'est nécessaire parce qu'on écrit dans /sys, qui contient des réglages système.
if [ "$EUID" -ne 0 ]; then
    echo "Il faut lancer ce script avec sudo :"
    echo "sudo ./setup_cpu.sh"
    exit 1
fi

echo "=== Setup CPU pour les benchmarks ==="
echo

# ------------------------------------------------------------
# Partie 1 : intel_pstate
# ------------------------------------------------------------
#
# intel_pstate est un driver utilisé sur beaucoup de CPU Intel
# pour gérer la fréquence, le turbo et les performances.
#
# Les fichiers min_perf_pct et max_perf_pct utilisent des POURCENTAGES.
#
# Donc ici :
#   min_perf_pct = 100
#   max_perf_pct = 100
#
# veut dire :
#   performance minimale = 100 %
#   performance maximale = 100 %
#
# Cela ne correspond pas directement à une fréquence en Hz.
# C'est une interface propre à intel_pstate.
# ------------------------------------------------------------

if [ -d /sys/devices/system/cpu/intel_pstate ]; then
    echo "intel_pstate détecté"

    # no_turbo contrôle le turbo boost.
    #
    # no_turbo = 1  => turbo désactivé
    # no_turbo = 0  => turbo autorisé
    #
    # On désactive le turbo pour éviter que certains tests soient plus rapides
    # juste parce que le CPU est monté temporairement en turbo.
    if [ -e /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
        echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
        echo "Turbo désactivé"
    fi

    # Ici, on force la plage de performance à 100%.
    #
    # Le but est d'éviter que le CPU descende en performance pendant les tests.
    # Cela aide à avoir des temps plus stables.
    if [ -f /sys/devices/system/cpu/intel_pstate/max_perf_pct ]; then
        echo 100 > /sys/devices/system/cpu/intel_pstate/max_perf_pct
        echo 100 > /sys/devices/system/cpu/intel_pstate/min_perf_pct
        echo "min_perf_pct et max_perf_pct mis à 100"
    fi

    echo
    echo "Valeurs actuelles de intel_pstate :"

    # grep "" sur tous les fichiers permet juste d'afficher leur contenu
    # avec le nom du fichier devant.
    #
    # Le 2>/dev/null évite d'afficher les erreurs si certains fichiers
    # ne sont pas lisibles.
    #
    # Le || true évite que le script s'arrête si grep rencontre un fichier
    # qu'il ne peut pas lire.
    grep "" /sys/devices/system/cpu/intel_pstate/* 2>/dev/null || true
    echo
fi

# ------------------------------------------------------------
# Partie 2 : cpufreq
# ------------------------------------------------------------
#
# cpufreq est une autre interface Linux pour gérer les fréquences CPU.
#
# Ici, contrairement à intel_pstate, les fichiers scaling_min_freq
# et scaling_max_freq ne prennent PAS des pourcentages.
#
# Ils prennent des fréquences en kHz.
#
# Exemple :
#   2400000 = 2 400 000 kHz = 2.4 GHz
#
# Donc ici, on ne met pas 100.
# On lit la fréquence maximale disponible, puis on met :
#
#   scaling_min_freq = maxfreq
#   scaling_max_freq = maxfreq
#
# Le but est de forcer le CPU à rester à une fréquence stable.
# ------------------------------------------------------------

if [ -d /sys/devices/system/cpu/cpufreq ]; then
    echo "cpufreq détecté"
    echo

    # Il peut y avoir plusieurs policies :
    # policy0, policy1, etc.
    #
    # Une policy peut correspondre à un coeur ou à un groupe de coeurs.
    # On applique donc les réglages à toutes les policies disponibles.
    for p in /sys/devices/system/cpu/cpufreq/policy[0-9]*; do
        echo "Configuration de $p"

        # On choisit la fréquence à utiliser.
        #
        # base_frequency correspond souvent à la fréquence nominale/base du CPU.
        # Elle peut être plus stable pour des benchmarks que cpuinfo_max_freq,
        # qui peut parfois correspondre à une valeur avec turbo.
        #
        # Si base_frequency n'existe pas, on prend cpuinfo_max_freq.
        if [ -f "$p/base_frequency" ]; then
            maxfreq=$(cat "$p/base_frequency")
            echo "Fréquence choisie depuis base_frequency : $maxfreq"
        else
            maxfreq=$(cat "$p/cpuinfo_max_freq")
            echo "Fréquence choisie depuis cpuinfo_max_freq : $maxfreq"
        fi

        # scaling_available_governors liste les governors disponibles.
        #
        # Un governor est une stratégie de gestion de la fréquence.
        #
        # Exemples :
        #   powersave    => privilégie l'économie d'énergie
        #   performance  => privilégie les performances
        #   userspace    => permet de fixer la fréquence directement
        #
        # Si userspace est disponible, c'est pratique parce qu'on peut écrire
        # directement la fréquence souhaitée dans scaling_setspeed.
        if grep -q userspace "$p/scaling_available_governors"; then
            echo userspace > "$p/scaling_governor"
            echo "$maxfreq" > "$p/scaling_setspeed"

            echo "Governor mis en userspace"
            echo "Fréquence fixée à $maxfreq avec scaling_setspeed"

        else
            # Si userspace n'est pas disponible, on utilise performance.
            #
            # performance demande au système de privilégier les performances
            # plutôt que l'économie d'énergie.
            echo performance > "$p/scaling_governor"
            echo "Governor mis en performance"

            # Ici, on essaie de fixer la fréquence min et max à la même valeur.
            #
            # Si min = max, le CPU a moins de liberté pour varier sa fréquence.
            # Cela rend les mesures plus stables.
            if [ -f "$p/scaling_max_freq" ]; then
                echo "$maxfreq" > "$p/scaling_max_freq"
                echo "$maxfreq" > "$p/scaling_min_freq"

                echo "scaling_min_freq et scaling_max_freq fixées à $maxfreq"
            fi

            # energy_performance_preference permet de donner une préférence
            # au CPU entre économie d'énergie et performance.
            #
            # Pour un benchmark, on met performance.
            if [ -e "$p/energy_performance_preference" ]; then
                echo performance > "$p/energy_performance_preference"
                echo "Préférence énergie mise sur performance"
            fi
        fi

        echo
    done
fi

echo "=== Setup terminé ==="