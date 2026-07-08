import os
import re
import pandas as pd
import matplotlib
matplotlib.use("TkAgg")
import matplotlib.pyplot as plt


# Dossier dans lequel je veux stocker mes graphes
GRAPH_DIR = "graphs"


def get_next_filename(base_name, extension="png"):
    """
    Cette fonction pertmet de créer un nouveau nom de fichier
    sans écraser les anciens graphes.

    même chose que dans graph_simple.py en gros
    """

    # On crée le dossier graphs s'il n'existe pas encore
    os.makedirs(GRAPH_DIR, exist_ok=True)

    # Fichier caché qui garde en mémoire la dernière version utilisée
    counter_file = os.path.join(GRAPH_DIR, f".{base_name}_counter")

    # Si le compteur existe déjà, on continue à partir de lui
    if os.path.exists(counter_file):
        with open(counter_file, "r") as f:
            content = f.read().strip()

        if content:
            last_version = int(content)
        else:
            last_version = -1

        next_version = last_version + 1

    # Si le compteur n'existe pas encore, on regarde les fichiers déjà présents
    # pour ne pas écraser un ancien graphe.
    else:
        pattern = re.compile(rf"^{re.escape(base_name)}_v(\d+)\.{extension}$")
        versions = []

        for filename in os.listdir(GRAPH_DIR):
            match = pattern.match(filename)

            if match:
                versions.append(int(match.group(1)))

        if versions:
            next_version = max(versions) + 1
        else:
            next_version = 0

    # Sécurité : si jamais le fichier existe déjà, on incrémente encore
    output_file = os.path.join(GRAPH_DIR, f"{base_name}_v{next_version}.{extension}")

    while os.path.exists(output_file):
        next_version += 1
        output_file = os.path.join(GRAPH_DIR, f"{base_name}_v{next_version}.{extension}")

    # On met à jour le compteur avec la dernière version utilisée
    with open(counter_file, "w") as f:
        f.write(str(next_version))

    return output_file


df_size = pd.read_csv("results_sizes_loop.csv")
df_sends = pd.read_csv("results_sends_loop.csv")


# ------------------------------------------------------------
# 1. Graphe selon la taille du buffer

plt.figure()

for program in df_size["program"].unique():
    sub = df_size[df_size["program"] == program].sort_values("buffer_size")

    y = sub["avg_elapsed_time_us"]
    yerr_lower = sub["avg_elapsed_time_us"] - sub["min_elapsed_time_us"]
    yerr_upper = sub["max_elapsed_time_us"] - sub["avg_elapsed_time_us"]

    plt.errorbar(
        sub["buffer_size"],
        y,
        yerr=[yerr_lower, yerr_upper],
        marker="o",
        capsize=5,
        label=program
    )

#plt.xscale("log")
plt.xlabel("Buffer size (bytes)")
plt.ylabel("Average elapsed time (us)")
plt.title("Average elapsed time depending on buffer size for 500 loop exchanges")
plt.legend()
plt.grid(True)

output_file = get_next_filename("loop_graph_buffer_size")
plt.savefig(output_file, dpi=300, bbox_inches="tight")
print(f"Graph saved in {output_file}")

plt.show()


# ------------------------------------------------------------
# 2. Graphe selon le nombre de loop exchanges

#pareil que pour le simple, on prend le temps total 

plt.figure()

for program in df_sends["program"].unique():
    sub = df_sends[df_sends["program"] == program].sort_values("nb_sends")

    y = sub["total_elapsed_time_us"]

    plt.plot(
        sub["nb_sends"],
        y,
        marker="o",
        label=program
    )

#plt.xscale("log")
plt.xlabel("Number of loop exchanges")
plt.ylabel("Total elapsed time (s)")
plt.title("Total elapsed time depending on number of loop exchanges with a buffer size of 65536 bytes")
plt.legend()
plt.grid(True)

output_file = get_next_filename("loop_graph_nb_sends_total")
plt.savefig(output_file, dpi=300, bbox_inches="tight")
print(f"Graph saved in {output_file}")

plt.show()