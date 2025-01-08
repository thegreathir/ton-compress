"""
Train a FC model by comparing its output with the LAST timestep of a pre-trained LSTM model's output.
When evaluating, the FC model is compared with the real next bit.

Usage:
  python train_lstm_fc.py --json_path data.json --lstm_model_path best_bit_lstm_model.pth
"""

import argparse
import json
import base64
import torch
import torch.nn as nn
import tqdm
import random
import os
from torch.utils.data import Dataset, DataLoader, random_split


# =========================
# 1. Dataset Class
# =========================
class Base64BitsFCDataset(Dataset):
    """
    Same dataset logic as in train_fc.py / train_lstm.py:
      - For each base64 entry, decode to bits.
      - For each (i in [0..L-2]), produce a sample:
         prefix = bits[:i+1] (padded left to 256 bits if needed)
         label  = bits[i+1]  (the next bit).
    """

    def __init__(self, json_path, seq_length=256):
        super().__init__()
        self.seq_length = seq_length

        with open(json_path, "r") as f:
            data = json.load(f)

        all_samples = []
        for item in tqdm.tqdm(data, desc="Processing JSON", unit="item"):
            if "content" not in item:
                continue
            if "special" in item and item["special"]:
                # skip 'special' entries if that's your logic
                continue

            b64body = item["content"]
            try:
                body_bytes = base64.b64decode(b64body)
            except Exception:
                continue

            bits = []
            for byte in body_bytes:
                for i in range(8):
                    bit = (byte >> (7 - i)) & 1
                    bits.append(bit)

            if len(bits) <= 1:
                continue

            L = len(bits)
            for i in range(L - 1):
                prefix = bits[: i + 1]
                label_bit = bits[i + 1]

                if len(prefix) < self.seq_length:
                    padded_prefix = [0] * (self.seq_length - len(prefix)) + prefix
                else:
                    padded_prefix = prefix[-self.seq_length :]

                all_samples.append((padded_prefix, label_bit))

        self.samples = all_samples
        print(f"Total samples: {len(self.samples)}")

    def __len__(self):
        return len(self.samples)

    def __getitem__(self, idx):
        prefix, label_bit = self.samples[idx]
        x_tensor = torch.tensor(prefix, dtype=torch.float32)
        y_tensor = torch.tensor(label_bit, dtype=torch.float32)
        return x_tensor, y_tensor


# =========================
# 2. LSTM Model
# =========================
class BitLSTM(nn.Module):
    """
    A simple LSTM model that predicts the next bit (0/1).
    - Input size = 1 (since we feed a single bit at each time step)
    - Hidden size = user-defined
    - Output size = 1 (logit for next bit)
    """

    def __init__(self, hidden_size=128, num_layers=1):
        super().__init__()
        self.hidden_size = hidden_size
        self.num_layers = num_layers

        # LSTM: input shape is (batch, seq_len, input_size)
        self.lstm = nn.LSTM(
            input_size=1,
            hidden_size=hidden_size,
            num_layers=num_layers,
            batch_first=True,
        )

        # Final linear layer to map LSTM hidden state -> 1-dimensional output
        self.fc = nn.Linear(hidden_size, 1)

    def forward(self, x):
        """
        Args:
          x: shape (batch_size, seq_length, 1)
        Returns:
          logits: shape (batch_size, seq_length, 1)
        """
        lstm_out, _ = self.lstm(x)  # (batch, seq_length, hidden_size)
        logits = self.fc(lstm_out)  # (batch, seq_length, 1)
        return logits


# =========================
# 3. FC Model (unchanged)
# =========================
class BitFC(nn.Module):
    """
    A simple fully-connected network for bit prediction.
    Input size = seq_length (256).
    Output size = 1 (logit for next bit).

    Must be the same as in train_fc.py (unchanged).
    """

    def __init__(self, seq_length=256, hidden_size=128):
        super().__init__()
        self.seq_length = seq_length
        self.hidden_size = hidden_size

        self.net = nn.Sequential(
            nn.Linear(seq_length, hidden_size),
            nn.ReLU(),
            nn.Linear(hidden_size, hidden_size),
            nn.ReLU(),
            nn.Linear(hidden_size, 1),
        )

    def forward(self, x):
        """
        x: shape (batch_size, seq_length)
        Returns logits: shape (batch_size, 1)
        """
        return self.net(x)


# =========================
# 4. Training Functions
# =========================


def train_one_epoch_fc_vs_lstm_last(
    fc_model, lstm_model, dataloader, criterion, optimizer, device="cpu"
):
    """
    Train the FC model by comparing its output to the *last timestep* of the LSTM output.
    - We freeze LSTM so it won't update.
    - For each batch:
        1) Reshape x into (batch, seq_len, 1) to feed LSTM
        2) LSTM outputs shape (batch, seq_len, 1). We extract the last element: logits[:, -1, :]
        3) FC outputs a single logit for each sample (batch, 1)
        4) Compare (FC_out) to (LSTM_out_last) using 'criterion' (e.g. MSE).

    Return average training loss.
    """
    fc_model.train()
    lstm_model.eval()  # ensure LSTM is frozen
    total_loss = 0.0

    for x, _ in tqdm.tqdm(dataloader, desc="Training (FC vs. LSTM last)", unit="batch"):
        x = x.to(device)
        # (batch, seq_length) -> (batch, seq_length, 1) for LSTM
        x_lstm = x.unsqueeze(-1)  # shape (batch, seq_length, 1)

        # Get LSTM output (don't compute grads for LSTM)
        with torch.no_grad():
            lstm_out_seq = lstm_model(x_lstm)  # (batch, seq_length, 1)
            lstm_out_last = lstm_out_seq[:, -1, :]  # (batch, 1)

        # FC output
        fc_out = fc_model(x)  # shape (batch, 1)

        # Compare FC to the last element of LSTM
        loss = criterion(fc_out, lstm_out_last)
        optimizer.zero_grad()
        loss.backward()
        optimizer.step()

        total_loss += loss.item()

    return total_loss / len(dataloader)


def evaluate_fc_on_real_bits(fc_model, dataloader, bce_criterion, device="cpu"):
    """
    Evaluate the FC model on real bits using BCEWithLogitsLoss.
    - For each batch:
      1) FC outputs a single logit (batch,1)
      2) Compare FC logits to the real next bit
    """
    fc_model.eval()
    total_loss = 0.0

    with torch.no_grad():
        for x, y in tqdm.tqdm(
            dataloader, desc="Evaluating (FC vs. real bits)", unit="batch"
        ):
            x = x.to(device)
            y = y.to(device).unsqueeze(-1)  # (batch, 1)

            fc_logits = fc_model(x)  # (batch, 1)
            loss = bce_criterion(fc_logits, y)
            total_loss += loss.item()

    return total_loss / len(dataloader)


# =========================
# 5. Main Script
# =========================
def main(args):
    # Hyperparameters
    json_path = args.json_path
    lstm_model_path = args.lstm_model_path
    seq_length = 192
    batch_size = 64
    hidden_size_lstm = 48  # Adjust if your LSTM used a different hidden size
    hidden_size_fc = 20  # Adjust if desired
    num_epochs = 100
    lr = 1e-3
    device = "cuda" if torch.cuda.is_available() else "cpu"
    print(f"Using device: {device}")

    # 1) Create dataset & DataLoader
    dataset = Base64BitsFCDataset(json_path, seq_length=seq_length)
    if len(dataset) == 0:
        print("Dataset is empty or insufficient. Exiting...")
        return

    val_ratio = 0.1
    val_size = int(len(dataset) * val_ratio)
    train_size = len(dataset) - val_size
    train_dataset, val_dataset = random_split(dataset, [train_size, val_size])

    train_loader = DataLoader(train_dataset, batch_size=batch_size, shuffle=True)
    val_loader = DataLoader(val_dataset, batch_size=batch_size, shuffle=False)

    # 2) Load the pre-trained LSTM model & freeze
    lstm_model = BitLSTM(hidden_size=hidden_size_lstm).to(device)
    lstm_model.load_state_dict(torch.load(lstm_model_path, map_location=device))
    lstm_model.eval()
    for param in lstm_model.parameters():
        param.requires_grad = False

    # 3) Instantiate the FC model
    fc_model = BitFC(seq_length=seq_length, hidden_size=hidden_size_fc).to(device)

    # We'll save our best FC model here
    best_checkpoint_path = "best_bit_fc_model_trained_from_lstm.pth"

    # Criterion to train FC vs. last LSTM output
    mimic_criterion = nn.MSELoss()
    # Criterion to evaluate FC vs. real bit
    bce_criterion = nn.BCEWithLogitsLoss()

    optimizer = torch.optim.Adam(fc_model.parameters(), lr=lr)

    best_val_loss = float("inf")

    # 4) Training Loop
    for epoch in range(num_epochs):
        print(f"Epoch {epoch + 1}/{num_epochs}")

        # Train FC by mimicking the last element of LSTM
        train_loss = train_one_epoch_fc_vs_lstm_last(
            fc_model,
            lstm_model,
            train_loader,
            mimic_criterion,
            optimizer,
            device=device,
        )

        # Evaluate FC on real bits
        val_loss = evaluate_fc_on_real_bits(
            fc_model, val_loader, bce_criterion, device=device
        )

        print(
            f"  [Train MSE vs LSTM last]: {train_loss:.4f} | [Val BCE vs real bits]: {val_loss:.4f}"
        )

        # Check if this is the best model so far
        if val_loss < best_val_loss:
            best_val_loss = val_loss
            torch.save(fc_model.state_dict(), best_checkpoint_path)
            print(f"  [*] New best FC model saved with val_loss={val_loss:.4f}")

    print(f"Training complete. Best validation loss: {best_val_loss:.4f}")
    print(f"Best FC model is stored at '{best_checkpoint_path}'")

    # 5) Load best model and export TorchScript
    fc_model.load_state_dict(torch.load(best_checkpoint_path, map_location=device))
    fc_model.eval()
    fc_model.to("cpu")

    example_input = torch.zeros((1, seq_length), dtype=torch.float32)
    scripted_model = torch.jit.script(fc_model, example_inputs=(example_input,))
    torchscript_path = "best_bit_fc_model_from_lstm_jit.pt"
    scripted_model.save(torchscript_path)
    print(f"Saved TorchScript model to: {torchscript_path}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--json_path",
        type=str,
        required=True,
        help="Path to the JSON file containing base64-encoded bodies.",
    )
    parser.add_argument(
        "--lstm_model_path",
        type=str,
        required=True,
        help="Path to the file containing the best LSTM model checkpoint.",
    )
    args = parser.parse_args()

    main(args)
