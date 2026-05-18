---
sidebar_position: 1
title: "First Steps with ChronoLog"
---

# First Steps with ChronoLog

Welcome to ChronoLog! We're so excited to have you here. ChronoLog is a powerful open-source project, proudly funded by the NSF, that aims to make managing and processing log data simple, scalable, and high-performance. Whether you're working with edge computing or high-performance computing (HPC) systems, ChronoLog has got your back. Let's dive in and get you up and running in no time!

## What is ChronoLog?

Imagine you have a flood of data coming at you from all directions: activities, events, logs — all the stuff you need to make sense of. ChronoLog steps in as your superhero, a distributed shared log store designed to:

- Scale effortlessly to handle huge volumes of data.
- Deliver blazing-fast performance.
- Adapt to a wide variety of use cases.

From edge devices to massive HPC systems, ChronoLog ensures you have a reliable and efficient way to organize and analyze your data. Sounds cool, right? Let's get you started with your very first ChronoLog setup!

---

## Getting Started: Setting Up ChronoLog

We've made sure to keep the setup as straightforward as possible. Just follow these steps, and you'll be ready to roll!

---

### Step 0: Prerequisites

Before you begin, make sure you have the following installed on your system:

- **Git** – Required for cloning the ChronoLog repository. Install it with:

  ```bash
  sudo apt install git  # Debian/Ubuntu
  ```

- **g++** – The GNU C++ compiler is needed to build ChronoLog. Install it with:

  ```bash
  sudo apt install g++  # Debian/Ubuntu
  ```

Once these are installed, you're ready to move on to setting up ChronoLog!

---

### Step 1: Clone the Repository

First, let's grab the ChronoLog repository. Open your terminal on the desired folder and run:

```bash
git clone https://github.com/grc-iit/ChronoLog.git
```

This will download the project to your local machine. Now you've got all the code you need!

---

### Step 2: Install the Spack Package Manager

Spack is an awesome tool for managing software builds. To get Spack, run:

```bash
git clone --branch v0.21.2 https://github.com/spack/spack.git
source /path-to-where-spack-was-cloned/spack/share/spack/setup-env.sh
```

Once it's installed, you're one step closer to launching ChronoLog.

---

### Step 3: Move to the Deploy Folder

Navigate to the "deploy" folder inside the ChronoLog repository:

```bash
cd /path-to-repo/deploy
```

Replace `/path-to-repo/` with the actual path to where you cloned the ChronoLog repository.

---

### Step 4: Build the Project

It's time to build ChronoLog. In the deploy folder, run:

```bash
./local_single_user_deploy.sh -b -t Debug
```

This will compile the project in Debug mode. Don't worry if it takes a little while; ChronoLog is getting ready to do big things!

---

### Step 5: Install the Project

Now, let's install ChronoLog to your local machine. Run:

```bash
./local_single_user_deploy.sh -i -w /home/$USER/chronolog/Debug
```

This sets up ChronoLog in a local directory so you can use it.

---

### Step 6: Deploy the Project

You're almost there! To deploy ChronoLog, simply run:

```bash
./local_single_user_deploy.sh -d -w /home/$USER/chronolog/Debug
```

ChronoLog is now deployed and ready for action.

---

### Step 7: Check the Installation

Let's make sure everything is running smoothly. Use this command to verify:

```bash
pgrep -laf "chronovisor_server|chrono_grapher|chrono_keeper|chrono_player"
```

If you see 4 processes like the ones listed on the command, congratulations! ChronoLog is successfully installed and deployed on your system. You're ready to start exploring its capabilities.

---

## Running the Interactive Client Admin

Now that you've got ChronoLog installed and deployed, it's time to explore its capabilities with the **Interactive Client Admin**. This tool lets you interact with the ChronoLog system in real time, and it's perfect for getting hands-on experience.

### Step 8: Launch the Interactive Client Admin

Let's fire up the interactive client! Open your terminal, navigate to the /bin directory on the location where ChronoLog is installed, and run:

```bash
client_admin -i ../conf/visor_conf.json
```

This will launch the interactive interface, where you can manage **Chronicles**, **Stories**, and **Events** directly.

---

### Step 9: Create Your First Chronicle

Chronicles are the core of ChronoLog's architecture. To create a Chronicle, simply type:

```bash
-c my_chronicle
```

This creates a new Chronicle named `my_chronicle`. Feel free to replace `my_chronicle` with a name of your choice.

---

### Step 10: Acquire a Story in Your Chronicle

Stories are individual data streams within a Chronicle. Story needs to be acquired before being accessed. To acquire a Story, type:

```bash
-a -s my_chronicle my_story
```

This creates a Story named `my_story` in the Chronicle `my_chronicle`. Now you're ready to start logging events!

---

### Step 11: Write an Event

Events are the data payloads stored in a Story. To write an event, type:

```bash
-w "This is my first event in ChronoLog!"
```

Replace the text inside the quotation marks with your own message. Each event you write is logged in real time.

---

### Step 12: Release the Story

Once you've finished working with a Story, release it. Story needs to be released for peaceful and safe close. To do so:

```bash
-q -s my_chronicle my_story
```

This frees up resources while keeping the data intact.

---

### Step 13: Destroy a Story or Chronicle (Optional)

If you no longer need a Story or Chronicle, you can delete them. You can remove them by destroying them:

- To destroy a Story:

  ```bash
  -d -s my_chronicle my_story
  ```

- To destroy a Chronicle:

  ```bash
  -d -c my_chronicle
  ```

:::caution
This action is irreversible, so make sure you no longer need the data before proceeding!
:::

---

### Step 14: Disconnect from the Server

When you're done exploring, safely disconnect from the server by typing:

```bash
-disconnect
```

---

## What's Next?

Now that you've set up ChronoLog, you're all set to explore its powerful features. Keep an eye out for our next tutorial, where we'll guide you through running your first ChronoLog application and uncovering the magic it can bring to your data workflows.

Welcome to the ChronoLog community — we can't wait to see what you'll build!
